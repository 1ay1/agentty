#!/usr/bin/env python3
"""schema_conformance.py — is our codec algebra the schema's type algebra?

A protocol schema IS a type algebra, and our codec DSL already embeds it:

    record(...)            product      T ≅ Π fᵢ : Tᵢ
    sum_tagged("k", ...)   coproduct    T ≅ Σ (k = tagᵢ) × Tᵢ
    variant_codec(...)     untagged coproduct
    enum_codec(...)        finite sum of units   T ≅ 1 + 1 + …
    Maybe<U>               option       U + 1
    List<U>                free monoid  U*
    required / optional / defaulted / constant   — field MODALITY

So conformance is not "does this string appear somewhere" — it is whether
our algebra and the schema's agree, constructor for constructor. Grepping
for a field name answers a weaker question and gets it wrong in both
directions: a name mentioned in a comment passes, and a field declared but
never wired into the record() also passes (that one actually happened — a
mutation deleting a struct member survived a struct-body search because the
codec line below mentioned the name).

What this checks, per type we implement:

  1. PRODUCT SHAPE   every schema-required key is a field in the codec, and
                     every codec key exists in the schema (no invented keys)
  2. MODALITY        a schema-required key is not modelled `optional`
                     (that would let us omit a MUST field), and a
                     schema-optional key is not modelled `required`
                     (that would make us reject a legal peer message)
  3. CARRIER         the C++ member type encodes to the JSON type the
                     schema declares — a key can be present and correctly
                     required and still carry a string where the protocol
                     says integer
  4. SUM VALUES      every enum's codec carries exactly the schema's
                     allowed strings — no missing, no invented

  MCP   {"type":"string","enum":[…]}          two spellings of the same
  ACP   {"oneOf":[{"const":"…"}, …]}          finite sum; both handled

Usage:
    python3 tests/schema_conformance.py \
        --mcp …/modelcontextprotocol/schema/2026-07-28/schema.json \
        --acp …/agent-client-protocol/schema/v1/schema.json

Exit 0 = the algebras agree for every type we implement. Types we do not
implement are skipped: not modelling `ElicitResult` is a scope decision,
modelling it wrongly is a bug.
"""
import argparse
import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

# ── Reading OUR algebra ──────────────────────────────────────────────────


def cpp_sources(*rel_dirs):
    out = {}
    for rel in rel_dirs:
        base = ROOT / rel
        if not base.exists():
            continue
        for f in base.rglob("*.hpp"):
            out[f] = f.read_text(errors="replace")
    return out


def _balanced(blob, open_idx):
    """The {...} block starting at open_idx, brace-matched."""
    depth = 0
    for j in range(open_idx, len(blob)):
        if blob[j] == "{":
            depth += 1
        elif blob[j] == "}":
            depth -= 1
            if depth == 0:
                return blob[open_idx : j + 1]
    return blob[open_idx:]


def codec_body(sources, name):
    """Body of `CodecOf<name>` — the CODEC, never the struct.

    A field can be declared as a member and never wired into the record(),
    in which case it exists in C++ and is absent from the wire. That is the
    whole bug class; reading the struct would pass it.
    """
    for _, blob in sources.items():
        m = re.search(r"struct\s+CodecOf<\s*" + re.escape(name) + r"\s*>\s*\{", blob)
        if m:
            return _balanced(blob, m.end() - 1)
    return None


# One codec field: modality + wire key.
FIELD_RE = re.compile(
    r"\b(required|optional|defaulted|constant|meta)\s*\(\s*\"([^\"]+)\"")


def our_product(body):
    """{key: modality} for a record codec, or None if it isn't one."""
    if body is None:
        return None
    if "variant_codec" in body or "sum_tagged" in body or re.search(r"\bmatch\s*\(", body):
        return None            # a coproduct, not a product
    if "record<" not in body:
        return None            # hand-written codec; shape not inferable
    return {k: mod for mod, k in FIELD_RE.findall(body)}


def our_sum_values(body):
    """The wire strings of an enum codec, or None if it isn't one."""
    if body is None or "enum_codec" not in body:
        return None
    return sorted(set(re.findall(r"EnumMapping<[^>]+>\s*\{[^,]+,\s*\"([^\"]+)\"", body)))


# ── Reading the SCHEMA's algebra ─────────────────────────────────────────


def defs_of(schema):
    return schema.get("$defs") or schema.get("definitions") or {}


def schema_product(node):
    """(required_keys, all_keys) for an object type, or None."""
    if not isinstance(node, dict):
        return None
    props = node.get("properties")
    if not isinstance(props, dict):
        return None
    return set(node.get("required") or []), set(props)


def schema_sum_values(node):
    """Allowed strings of a finite string sum, in either spelling."""
    if not isinstance(node, dict):
        return None
    vals = node.get("enum")
    if isinstance(vals, list) and node.get("type") == "string":
        return sorted(str(v) for v in vals)
    one_of = node.get("oneOf")
    if isinstance(one_of, list):
        consts = [b["const"] for b in one_of
                  if isinstance(b, dict) and isinstance(b.get("const"), str)]
        # Only when EVERY arm is a string const — a oneOf mixing consts with
        # object shapes is a tagged union, not an enum.
        if consts and len(consts) == len(one_of):
            return sorted(consts)
    return None


# ── Comparing them ───────────────────────────────────────────────────────

# Framing the transport writes, not codec fields.
ENVELOPE = {"jsonrpc", "method", "id"}
# Schema bases every concrete type inherits from; each concrete type is
# checked on its own. We model them as nothing, so matching them only ever
# finds an unrelated `using Result = R;` in a template.
ABSTRACT = {"Result", "Request", "Notification", "PaginatedRequest",
            "PaginatedResult", "ClientRequest", "ClientNotification",
            "ServerRequest", "ServerNotification", "AgentRequest",
            "AgentNotification", "JSONRPCRequest", "JSONRPCNotification"}


def struct_members(sources, name):
    """{member: declared C++ type} for `struct <name> { ... };`.

    The codec gives MODALITY (required/optional/defaulted); the struct gives
    the CARRIER. Conformance needs both — a key can be present, correctly
    required, and still carry a string where the schema says integer, which
    encodes a JSON type the peer rejects.
    """
    for _, blob in sources.items():
        m = re.search(r"\bstruct\s+" + re.escape(name) + r"\b\s*(?::[^{]*)?\{", blob)
        if not m:
            continue
        body = _balanced(blob, m.end() - 1)
        out = {}
        # `Type name;` / `Type name = init;` — one member per statement.
        for decl in re.finditer(
                r"^\s*((?:const\s+)?[\w:]+(?:\s*<[^;]*?>)?)\s+(\w+)\s*(?:=[^;]*)?;",
                body, re.M):
            out[decl.group(2)] = decl.group(1).strip()
        return out
    return None


# Schema JSON type -> the C++ carriers that encode to it. Deliberately a
# whitelist: an unrecognised carrier is reported, not assumed fine. Better a
# note to widen the table than a silent pass on a real mismatch.
CARRIERS = {
    "string":  ("std::string", "string"),
    "integer": ("int", "int64", "int32", "size_t", "long", "unsigned"),
    "number":  ("double", "float", "int", "int64"),
    "boolean": ("bool",),
    "array":   ("List", "vector", "array"),
    "object":  ("Json", "map", "object"),
}


def carrier_ok(schema_type, cpp_type):
    """Could `cpp_type` encode to `schema_type`?

    Unwraps Maybe<>/List<> to the payload, since the modality and
    multiplicity dimensions are checked separately — here we only care what
    is ultimately carried.
    """
    t = cpp_type
    # A named newtype (ToolCallId, SessionId, MessageId, …) wraps a string.
    if re.fullmatch(r"\w*(Id|Name|Path|Uri|Version)", t):
        return schema_type == "string"
    inner = re.sub(r"^Maybe<(.*)>$", r"\1", t).strip()
    if schema_type == "array":
        return bool(re.match(r"(List|std::vector)\s*<", inner))
    inner = re.sub(r"^(List|std::vector)\s*<(.*)>$", r"\2", inner).strip()
    pats = CARRIERS.get(schema_type)
    if pats is None:
        return True            # union / unconstrained — nothing to check
    low = inner.lower()
    if any(p.lower() in low for p in pats):
        return True
    # A nested record or enum is a legitimate carrier for object/string — but
    # only if it is NOT a known scalar. Accepting any identifier here let
    # `std::string` pass for `type: object`, which is precisely the mismatch
    # worth catching: a scalar where the peer expects a record.
    SCALARS = re.compile(
        r"^(std::)?(string|string_view|bool|char|u?int\w*|int\d*_t|"
        r"uint\d*_t|size_t|double|float|long|short)$")
    if SCALARS.match(inner):
        return False
    if schema_type in ("string", "object") and re.fullmatch(r"[\w:]+", inner):
        return True
    return False


def resolve_ref(defs, node, depth=0):
    """Follow `$ref` to the definition it names.

    140 MCP properties are a bare `{"$ref": "#/$defs/Annotations"}` with no
    `type` of their own, so a checker that only reads `type` skips every one
    of them — silently, which is the failure mode this whole script exists
    to avoid. Resolving lets the carrier check see the target's shape.

    Depth-bounded: the schemas are recursive (a ContentBlock can nest a
    resource that nests content), and an unbounded chase would not
    terminate.
    """
    seen = 0
    while isinstance(node, dict) and "$ref" in node and seen < 8:
        target = node["$ref"].rsplit("/", 1)[-1]
        nxt = defs.get(target)
        if not isinstance(nxt, dict):
            return node, target
        node, seen = nxt, seen + 1
        if "$ref" not in node:
            return node, target
    return node, None


# Formats we understand. Two families:
#   STRING  byte (base64), uri, uri-template, date-time — all strings on the
#           wire, so the carrier check already covers them.
#   NUMERIC  double/float/int32/uint32/int64/uint64 — these are WIDTH hints
#           (OpenAPI style), not string formats, and they DO constrain the
#           carrier: `uint32` in a std::int64_t is fine, in a std::string is
#           not — which the carrier check catches via `type` anyway.
# Listed so an unknown format is visible rather than silently ignored.
KNOWN_FORMATS = {
    "byte", "uri", "uri-template", "date-time", "uuid",
    "double", "float", "int32", "uint32", "int64", "uint64",
}


# Runtime constraints the C++ type cannot express, enforced at a CHOKE POINT
# in code instead. Listed here so the checker stays quiet about them and the
# enforcement site is discoverable from one place — a note nobody acts on is
# decoration, but deleting the check would lose the contract entirely.
#
#   <Type>.<field>: where the clamp lives
ENFORCED_AT = {
    # `minimum: 0` on a ttl. Clamped in apply_cache_hint (stateless server)
    # and in the typed server's discover/list paths, so a negative can never
    # reach the wire regardless of what a caller passes.
    "DiscoverResult.ttlMs": "server_stateless.hpp apply_cache_hint + server.hpp",
    "ListToolsResult.ttlMs": "server_stateless.hpp apply_cache_hint",
    "ListPromptsResult.ttlMs": "server_stateless.hpp apply_cache_hint",
    "ListResourcesResult.ttlMs": "server_stateless.hpp apply_cache_hint",
    "ListResourceTemplatesResult.ttlMs": "server_stateless.hpp apply_cache_hint",
    "ReadResourceResult.ttlMs": "server_stateless.hpp apply_cache_hint",
}


def semantic_notes(tname, key, prop, cpp_type):
    """Constraints the TYPE cannot express.

    These are RUNTIME contracts, not shape mismatches, so they are notes
    rather than failures — and only when the carrier does not already
    enforce them.

    `minimum: 0` is the common one: C++ has no non-negative int, and
    wrapping 27 fields in a newtype would cost more than the bug. But an
    UNSIGNED carrier satisfies it outright, and so does a format that
    already says unsigned (`uint32`), so neither is worth reporting —
    reporting them anyway produced 20 lines of noise nobody would read,
    which is how a checker becomes decoration.
    """
    out = []
    lo = prop.get("minimum")
    fmt = prop.get("format")
    if isinstance(lo, (int, float)) and lo >= 0:
        unsigned_carrier = re.search(r"unsigned|size_t|uint", cpp_type, re.I)
        unsigned_format = isinstance(fmt, str) and fmt.startswith("uint")
        enforced = f"{tname}.{key}" in ENFORCED_AT
        if not unsigned_carrier and not unsigned_format and not enforced:
            out.append(f"{tname}.{key}: schema minimum={lo}, carrier "
                       f"{cpp_type} admits negatives (runtime contract)")
    if isinstance(fmt, str) and fmt not in KNOWN_FORMATS:
        out.append(f"{tname}.{key}: unrecognised format {fmt!r} — "
                   f"widen KNOWN_FORMATS after checking what it implies")
    return out


def check(label, schema_path, sources):
    schema = json.loads(Path(schema_path).read_text())
    defs = defs_of(schema)
    tags = set()
    for _, blob in sources.items():
        tags |= set(re.findall(r'sum_tagged<[^>]+>\s*\(\s*"([^"]+)"', blob))

    products = sums = 0
    fails = []
    notes = []

    for name in sorted(defs):
        if name in ABSTRACT:
            continue
        body = codec_body(sources, name)
        if body is None:
            continue                      # not implemented — scope decision
        node = defs[name]

        # ── finite sums ──
        want_vals = schema_sum_values(node)
        got_vals = our_sum_values(body)
        if want_vals is not None and got_vals is not None:
            sums += 1
            for v in want_vals:
                if v not in got_vals:
                    fails.append(f"{name}: enum missing {v!r}")
            for v in got_vals:
                if v not in want_vals:
                    fails.append(f"{name}: enum has {v!r}, not in schema")
            continue

        # ── products ──
        want = schema_product(node)
        got = our_product(body)
        if want is None or got is None:
            continue
        want_req, want_all = want
        products += 1

        for k in sorted(want_req - ENVELOPE - tags):
            mod = got.get(k)
            if mod is None:
                fails.append(f"{name}.{k}: REQUIRED by schema, absent from codec")
            elif mod == "optional":
                # `optional` encodes a Maybe and OMITS the key when empty —
                # for a MUST field that is a wire violation we would never
                # see locally, since our own decoder tolerates the absence.
                fails.append(f"{name}.{k}: required by schema, modelled `optional`")

        want_opt = want_all - want_req
        for k in sorted(want_opt):
            if got.get(k) == "required":
                # Mirror image: we would REJECT a legal message from a peer
                # that legitimately omits it.
                fails.append(f"{name}.{k}: optional in schema, modelled `required`")

        # ── carrier types ──
        # The fourth dimension. A key can be present, correctly required,
        # and still carry the wrong JSON type — ttlMs as a string, say,
        # which encodes `"3600000"` where the peer's schema says integer.
        # Modality and presence both pass that happily.
        members = struct_members(sources, name) or {}
        props = node.get("properties") or {}
        for k in sorted(set(got) & set(props) & set(members)):
            prop = props[k]
            if not isinstance(prop, dict):
                continue
            # Follow $ref FIRST. 140 MCP properties are a bare $ref with no
            # `type` of their own; reading `type` alone skips every one.
            target, refname = resolve_ref(defs, prop)
            st = target.get("type")
            if isinstance(st, list):
                st = next((x for x in st if x != "null"), None)   # nullable
            if isinstance(st, str):
                if not carrier_ok(st, members[k]):
                    via = f" (via {refname})" if refname else ""
                    fails.append(
                        f"{name}.{k}: schema says {st}{via}, codec carries "
                        f"{members[k]}")
            elif refname:
                # The $ref names a record. The carrier must be that record
                # (or a Maybe/List of it) — a std::string here would encode
                # a scalar where the peer expects an object.
                inner = re.sub(r"^Maybe<(.*)>$", r"\1", members[k]).strip()
                inner = re.sub(r"^(List|std::vector)\s*<(.*)>$", r"\2", inner).strip()
                if re.fullmatch(r"std::string|bool|int\w*|double|float", inner):
                    fails.append(
                        f"{name}.{k}: schema $ref {refname} (a record), "
                        f"codec carries scalar {members[k]}")
            notes.extend(semantic_notes(name, k, target, members[k]))

        extras = sorted(set(got) - want_all - ENVELOPE - tags - {"_meta"})
        if extras:
            # An extra key is a VIOLATION only when the schema closes the
            # record (`additionalProperties: false`). JSON Schema leaves it
            # open by default, and MCP/ACP both rely on that for forward
            # compatibility — `tasks` and `execution` are draft features we
            # model ahead of stabilisation, and a peer that doesn't know
            # them ignores the key.
            #
            # Reported either way, because an extra key is ALSO how a typo
            # looks ("temperatuer"), and silence there would hide it.
            closed = node.get("additionalProperties") is False
            for k in extras:
                msg = f"{name}.{k}: in codec, not in schema"
                if closed:
                    fails.append(msg + " (record is CLOSED — violation)")
                else:
                    notes.append(msg + " (record is open — extension)")

    print(f"[{label}] {products} product(s), {sums} sum(s) checked")
    for n in notes:
        print(f"  note: {n}")
    for f in fails:
        print(f"  {f}")
    return fails


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--mcp")
    ap.add_argument("--acp")
    a = ap.parse_args()

    # The schemas live upstream, not vendored — pinning a copy would defeat
    # the point, since the job is to notice when upstream MOVES.
    if not any(p and Path(p).exists() for p in (a.mcp, a.acp)):
        print("SKIP: no schema given (clone the spec repos, pass --mcp/--acp)")
        return 0

    bad = []
    if a.mcp and Path(a.mcp).exists():
        bad += check("mcp", a.mcp, cpp_sources("mcp-cpp/include"))
    if a.acp and Path(a.acp).exists():
        bad += check("acp", a.acp, cpp_sources("acp-cpp/include"))

    print()
    if bad:
        print(f"{len(bad)} conformance problem(s)")
        return 1
    # Four dimensions of the algebra, verified:
    #   product shape   required keys present, no invented keys
    #   modality        required/optional matches the schema
    #   carrier         the C++ type encodes to the schema's JSON type
    #   sum values      enums carry exactly the schema's strings
    #
    # NOT verified, and worth naming rather than implying otherwise:
    # semantic constraints (minimum/maxLength/pattern/format), $ref
    # chasing into nested records, and anything the schema expresses as an
    # untyped union. Those need a real schema walker, not a shape compare.
    print("codec algebra agrees with the schema algebra")
    print("  (product shape · field modality · carrier type · sum values)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
