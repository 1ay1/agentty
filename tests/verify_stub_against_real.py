#!/usr/bin/env python3
"""verify_stub_against_real.py — is tests/fake_local_server.py honest?

The stub encodes OUR READING of each server's API. If the reading is wrong,
every test built on it passes and the bug ships anyway — the test proves the
stub matches itself, not that it matches reality.

This closes that loop: point it at a REAL server and it diffs the shape the
stub claims against the shape the server actually returns. Key presence and
JSON types only, never values (those legitimately differ per model/host).

    python3 tests/verify_stub_against_real.py ollama   http://127.0.0.1:11434
    python3 tests/verify_stub_against_real.py llama    http://127.0.0.1:8080
    python3 tests/verify_stub_against_real.py lmstudio http://127.0.0.1:1234

Exit 0 = the stub is faithful for every route the real server serves.

There is also a --self-check mode, which runs every claim against the STUB
instead of a real server:

    python3 tests/verify_stub_against_real.py --self-check

That is a weaker question on purpose — it cannot tell you the stub matches
reality, only that the stub matches the claims. It exists because the two
used to drift silently in the SAME direction: a field added to the stub but
not to CLAIMS is never verified against anything, and a field in CLAIMS the
stub doesn't serve reports "skip" forever and looks fine. Wiring this into
ctest means adding a shape to the stub without stating the claim fails the
build, so the manual real-server run has something honest to check.
"""
import json
import os
import subprocess
import sys
import time
import urllib.error
import urllib.request

# What each mode claims, as {route: [(json path, expected type)]}.
# Paths use dots; "[]" means "first element of this array".
CLAIMS = {
    "ollama": {
        "GET /api/tags": [
            ("models[].name", str),
            ("models[].model", str),
        ],
        "GET /api/ps": [
            # The loaded window. Only present when a model is resident.
            ("models[].context_length", int),
            ("models[].model", str),
        ],
        "POST /api/show": [
            ("model_info", dict),
            # Matched by SUFFIX, so the test asserts the suffix exists.
            ("model_info.*.context_length", (int, float)),
        ],
    },
    "llama": {
        "GET /v1/models": [
            ("data[].id", str),
            ("data[].meta.n_ctx", int),
            ("data[].meta.n_ctx_train", int),
        ],
        "GET /props": [
            ("default_generation_settings.n_ctx", int),
            # The capability object. llama.cpp builds it by RUNNING the
            # model's jinja template against probe inputs and diffing the
            # output (common/jinja/caps.cpp, caps_get), so the key names
            # are fixed by caps::to_map() in that file. If upstream renames
            # one, agentty silently loses reasoning on every llama.cpp
            # model and falls back to guessing from the GGUF filename —
            # exactly the bug this replaced. Check the spelling, loudly.
            ("chat_template_caps", dict),
            ("chat_template_caps.supports_reasoning_effort", bool),
            ("chat_template_caps.supports_tool_calls", bool),
        ],
    },
    "lmstudio": {
        "GET /api/v1/models": [
            ("models[].key", str),
            ("models[].max_context_length", int),
            ("models[].loaded_instances[].config.context_length", int),
            # The capability object — the field the whole LM Studio bug was
            # in. `reasoning` is an OBJECT here ({allowed_options, default}),
            # not the bare bool Mistral uses, and the reader used to require
            # is_boolean() and skip anything else. Every model then fell
            # through to inference over a `publisher/model` key.
            #
            # Checked as dict on purpose: if LM Studio ever flattens it to a
            # bool, this fails loudly rather than the object arm quietly
            # never matching. Note only the reasoning-capable rows carry it,
            # so point this at a server with one loaded.
            ("models[].capabilities", dict),
            ("models[].capabilities.trained_for_tool_use", bool),
            # The OBJECT arm itself. Worth stating separately from the
            # parent: this exact type is what declared_reasoning()'s
            # truthy_object combinator exists for, and if LM Studio ever
            # flattens it to a bare bool the lens still matches (the bool
            # arm catches it) while every comment here becomes a lie. Pin
            # the shape so that change is visible rather than silent.
            ("models[].capabilities.reasoning", dict),
            ("models[].capabilities.reasoning.default", str),
        ],
    },
}

BODIES = {"POST /api/show": {"model": None}}   # model filled in at runtime


def fetch(base, route, model=None):
    method, path = route.split(" ", 1)
    url = base.rstrip("/") + path
    data = None
    if method == "POST":
        body = dict(BODIES.get(route, {}))
        if "model" in body:
            body["model"] = model
        data = json.dumps(body).encode()
    req = urllib.request.Request(
        url, data=data, method=method,
        headers={"Content-Type": "application/json"})
    with urllib.request.urlopen(req, timeout=10) as r:
        return json.loads(r.read())


def resolve(doc, path):
    """Walk a dotted path. '[]' takes the first element; '*' matches any key
    whose remainder matches the rest of the path (used for GGUF arch keys)."""
    cur = doc
    parts = path.split(".")
    i = 0
    while i < len(parts):
        p = parts[i]
        if p == "*":
            suffix = ".".join(parts[i + 1:])
            if not isinstance(cur, dict):
                return None, "expected object for '*'"
            for k, v in cur.items():
                if k.endswith("." + suffix) or k == suffix:
                    return v, None
            return None, f"no key ending in '.{suffix}'"
        if p.endswith("[]"):
            key = p[:-2]
            if key:
                if not isinstance(cur, dict) or key not in cur:
                    return None, f"missing '{key}'"
                cur = cur[key]
            if not isinstance(cur, list):
                return None, f"'{key}' is not a list"
            if not cur:
                return None, f"'{key}' is empty"
            cur = cur[0]
        else:
            if not isinstance(cur, dict) or p not in cur:
                return None, f"missing '{p}'"
            cur = cur[p]
        i += 1
    return cur, None


def verify(mode, base, quiet=False):
    """Check every claim for `mode` against the server at `base`.

    Returns (checked, bad). A route the server doesn't serve is SKIPPED,
    not counted — the point is "is what you do serve the shape we claim",
    and a real llama.cpp has no /api/ps to answer for.
    """
    claims = CLAIMS[mode]

    # /api/show needs a model name; take the first one the server lists.
    model = None
    if mode == "ollama":
        try:
            tags = fetch(base, "GET /api/tags")
            ms = tags.get("models") or []
            if ms:
                model = ms[0].get("model") or ms[0].get("name")
        except Exception:
            pass

    bad = 0
    checked = 0
    for route, fields in claims.items():
        try:
            doc = fetch(base, route, model)
        except urllib.error.HTTPError as e:
            if not quiet:
                print(f"  skip {route}: HTTP {e.code} (route not served here)")
            continue
        except Exception as e:
            if not quiet:
                print(f"  skip {route}: {e}")
            continue
        for path, want in fields:
            checked += 1
            got, err = resolve(doc, path)
            if err:
                # An absent optional (nothing loaded) is not a lie.
                print(f"  MISS {route} {path}: {err}")
                bad += 1
                continue
            if not isinstance(got, want):
                wname = getattr(want, "__name__", str(want))
                print(f"  TYPE {route} {path}: stub says {wname}, "
                      f"server sent {type(got).__name__} ({got!r})")
                bad += 1
            elif not quiet:
                print(f"  ok   {route} {path} = {got!r}")
    return checked, bad


# Claims that a real server answers but the STUB deliberately does not, with
# the reason. Without this list --self-check would force the stub to serve
# every field, which would destroy the cases it exists to cover.
SELF_CHECK_EXEMPT = {
    # The llama.cpp row carries only the train-time ceiling on purpose: the
    # served window comes from /props, and that gap IS the bug the window
    # probe guards. Adding n_ctx here would delete the test.
    ("llama", "GET /v1/models", "data[].meta.n_ctx"),
}


def self_check():
    """Run every claim against the stub instead of a real server."""
    here = os.path.dirname(os.path.abspath(__file__))
    stub = os.path.join(here, "fake_local_server.py")
    total_bad = 0
    port = 18400
    for mode in CLAIMS:
        port += 1
        proc = subprocess.Popen([sys.executable, stub, mode, str(port)],
                                stdout=subprocess.DEVNULL,
                                stderr=subprocess.DEVNULL)
        try:
            base = f"http://127.0.0.1:{port}"
            # Wait for the listener rather than sleeping a guessed amount.
            for _ in range(100):
                try:
                    urllib.request.urlopen(base + "/v1/models", timeout=0.2)
                    break
                except urllib.error.HTTPError:
                    break          # answered, just not that route
                except Exception:
                    time.sleep(0.05)
            print(f"== {mode}")
            checked, bad = verify(mode, base, quiet=True)
            exempt = sum(1 for (m, _r, _p) in SELF_CHECK_EXEMPT if m == mode)
            bad -= exempt
            if bad > 0:
                print(f"   {bad} claim(s) the stub does not serve")
                total_bad += bad
            else:
                print(f"   {checked} claims served"
                      + (f" ({exempt} exempt)" if exempt else ""))
        finally:
            proc.terminate()
            proc.wait(timeout=5)
    if total_bad:
        print(f"\n{total_bad} claim(s) unserved by the stub")
        print("either serve the shape in fake_local_server.py, or drop the")
        print("claim from CLAIMS — an unserved claim is verified by nothing")
        return 1
    print("\nstub serves every claim")
    return 0


def main():
    if len(sys.argv) >= 2 and sys.argv[1] == "--self-check":
        return self_check()
    if len(sys.argv) < 3:
        print(__doc__)
        return 2
    mode, base = sys.argv[1], sys.argv[2]
    if mode not in CLAIMS:
        print(f"unknown mode '{mode}' (have: {', '.join(CLAIMS)})")
        return 2

    checked, bad = verify(mode, base)
    print(f"\n{checked - bad}/{checked} claims verified against the real server")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
