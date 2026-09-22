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
"""
import json
import sys
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
        ],
    },
    "lmstudio": {
        "GET /api/v1/models": [
            ("models[].key", str),
            ("models[].max_context_length", int),
            ("models[].loaded_instances[].config.context_length", int),
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


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 2
    mode, base = sys.argv[1], sys.argv[2]
    claims = CLAIMS.get(mode)
    if not claims:
        print(f"unknown mode '{mode}' (have: {', '.join(CLAIMS)})")
        return 2

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
            print(f"  skip {route}: HTTP {e.code} (route not served here)")
            continue
        except Exception as e:
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
            else:
                print(f"  ok   {route} {path} = {got!r}")

    print(f"\n{checked - bad}/{checked} claims verified against the real server")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
