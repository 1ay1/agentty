#!/usr/bin/env python3
"""schema_conformance.py — do our C++ codecs carry every REQUIRED field?

The bug this exists to catch is silent and self-concealing: a required
field we never emit. Our own client defaults it, so agentty-talking-to-
agentty works perfectly; only a conformant third-party peer rejects the
response. No amount of testing against ourselves finds it. That is exactly
how `resultType` survived — required on eight MCP result types, emitted on
none, while every one of our own tests passed.

So this reads the SPEC's `required` arrays rather than our code, and checks
each named field appears in the corresponding C++ struct. It is a coverage
check, not a type check: it answers "is this field present at all", which is
the question the invisible-bug class actually turns on.

    python3 tests/schema_conformance.py \
        --mcp /path/to/modelcontextprotocol/schema/2026-07-28/schema.json \
        --acp /path/to/agent-client-protocol/schema/v1/schema.json

Exit 0 = every required field of every type we implement is present.
Types we do not implement at all are reported as skipped, not failed — not
implementing `ElicitResult` is a scope decision; implementing it without
`action` is a bug.
"""
import argparse
import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


def cpp_sources(*rel_dirs):
    """All headers that could define a codec, as one blob per struct name."""
    text = {}
    for rel in rel_dirs:
        base = ROOT / rel
        if not base.exists():
            continue
        for f in base.rglob("*.hpp"):
            text[f] = f.read_text(errors="replace")
    return text


def required_fields(schema):
    """{TypeName: [required field names]} for every type that declares any."""
    defs = schema.get("$defs") or schema.get("definitions") or {}
    out = {}
    for name, node in defs.items():
        req = node.get("required")
        if isinstance(req, list) and req:
            out[name] = req
    return out


def codec_body(sources, name):
    """The body of `CodecOf<name>::get()`, or None if we define no codec.

    THE CODEC, not the struct. A field can be declared as a member and never
    wired into the record(), in which case it exists in C++ and is absent
    from the wire — which is the failure this whole script is about. Checking
    the struct body would pass that; checking the codec is what actually
    answers "does this key get emitted".

    (Found the hard way: a mutation that deleted the `resultType` MEMBER
    still passed a struct-body check, because the codec line below it
    mentioned the name.)
    """
    for _, blob in sources.items():
        m = re.search(
            r"struct\s+CodecOf<\s*" + re.escape(name) + r"\s*>\s*\{", blob)
        if not m:
            continue
        i = m.end() - 1
        depth = 0
        for j in range(i, len(blob)):
            if blob[j] == "{":
                depth += 1
            elif blob[j] == "}":
                depth -= 1
                if depth == 0:
                    return blob[i : j + 1]
        return blob[i:]
    return None


def tagged_union_keys(sources):
    """Discriminator keys emitted by the VARIANT, not by any member struct.

    `sum_tagged<ContentBlock>("type", …)` writes `"type":"text"` when it
    encodes a TextContent, so TextContent itself has no `type` member. A
    plain struct-body search would report every arm of every tagged union as
    missing its discriminator — 13 false positives here, which is exactly the
    noise that makes a conformance checker get ignored.
    """
    keys = set()
    for _, blob in sources.items():
        for m in re.finditer(r'sum_tagged<[^>]+>\s*\(\s*"([^"]+)"', blob):
            keys.add(m.group(1))
    return keys


def check(label, schema_path, sources, ignore_fields=frozenset()):
    schema = json.loads(Path(schema_path).read_text())
    reqs = required_fields(schema)
    ignore = set(ignore_fields) | tagged_union_keys(sources)

    implemented = missing = skipped = 0
    failures = []

    for tname, fields in sorted(reqs.items()):
        # `Result` and `Request` are the schema's ABSTRACT bases — every
        # concrete type inherits their required fields, and each concrete
        # type is checked on its own below. We model them as nothing (there
        # is no `struct Result`), so matching them here only ever finds an
        # unrelated `using Result = R;` in a template.
        if tname in ("Result", "Request", "Notification",
                     "PaginatedRequest", "PaginatedResult"):
            skipped += 1
            continue
        body = codec_body(sources, tname)
        if body is None:
            skipped += 1
            continue
        # A VARIANT codec carries no keys of its own — it dispatches to an
        # arm, and the arm's codec carries the required fields (checked in
        # its own right, since each arm is a named type too). `sum_tagged`,
        # `variant_codec`, and the hand-written `match(...)` dispatch in
        # acp-cpp's EmbeddedResource all have this shape.
        if ("variant_codec" in body or "sum_tagged" in body
                or re.search(r"\bmatch\s*\(", body)):
            skipped += 1
            continue
        implemented += 1
        for f in fields:
            if f in ignore:
                continue
            # The codec spells the JSON key as a STRING literal, so look for
            # exactly that — `"resultType"` — not a bare identifier. `_meta`
            # is spelled that way in the codec too.
            if f'"{f}"' not in body:
                failures.append(f"{tname}.{f}")
                missing += 1

    print(f"[{label}] {implemented} implemented types checked, "
          f"{skipped} not implemented (skipped)")
    for f in failures:
        print(f"  MISSING required field: {f}")
    return failures


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--mcp", help="path to MCP schema.json")
    ap.add_argument("--acp", help="path to ACP schema.json")
    args = ap.parse_args()

    # The schemas live in their upstream repos, not vendored here — pinning a
    # copy would defeat the point, since the whole job is to notice when
    # UPSTREAM moves. So: no schema, no check, exit 0. This is opt-in
    # tooling, not a gate that fails on a machine without the clones.
    have = [p for p in (args.mcp, args.acp) if p and Path(p).exists()]
    if not have:
        print("SKIP: no schema given (clone the spec repos and pass --mcp/--acp)")
        print("  MCP: github.com/modelcontextprotocol/modelcontextprotocol"
              "  → schema/<version>/schema.json")
        print("  ACP: github.com/agentclientprotocol/agent-client-protocol"
              "  → schema/v1/schema.json")
        return 0

    bad = []
    if args.mcp and Path(args.mcp).exists():
        src = cpp_sources("mcp-cpp/include")
        # `jsonrpc` and `method` are framing the transport writes, not codec
        # fields; `id` likewise. They are required in the JSON-RPC envelope
        # types, which we build rather than model as structs.
        bad += check("mcp", args.mcp, src,
                     ignore_fields={"jsonrpc", "method", "id"})
    if args.acp and Path(args.acp).exists():
        src = cpp_sources("acp-cpp/include")
        bad += check("acp", args.acp, src,
                     ignore_fields={"jsonrpc", "method", "id"})

    print()
    if bad:
        print(f"{len(bad)} required field(s) missing")
        return 1
    print("every required field of every implemented type is present")
    return 0


if __name__ == "__main__":
    sys.exit(main())
