#pragma once
// agentty/provider/openai/dialect.hpp — the observation tables. SSOT.
//
// Every way the "OpenAI-compatible" wire is spelled in the wild lives HERE,
// as a declaration, and nowhere else. The transport observes through these
// lenses; it does not re-derive a field name at a call site.
//
// WHY A TABLE AND NOT `if` BRANCHES: the deviations are not edge cases, they
// are the normal state of an ecosystem with no specification (see lens.hpp for
// why there is no spec to conform to). Spread across a 3000-line decoder they
// are invisible, untestable in isolation, and each new provider adds another
// branch someone has to find. As data they are a list you can read in one
// screen, replay against captured fixtures, and extend with one line.
//
// EVERY ENTRY BELOW IS A CLAIM ABOUT AN OBSERVED PROVIDER, not a guess. When
// you add one, say which endpoint you saw it on.

#include "agentty/provider/openai/lens.hpp"

namespace agentty::provider::openai::dialect {

// ── reasoning / chain-of-thought text ────────────────────────────────────
//
// Reasoning-capable models stream thinking in a field PARALLEL to `content`.
// There is no agreement on its name:
//
//   reasoning_content  DeepSeek introduced it on Chat Completions; vLLM
//                      followed, so most self-hosted and most "compat"
//                      gateways use it. (Verified: DeepSeek, vLLM.)
//   reasoning          OpenAI's Responses API, some OpenRouter passthroughs,
//                      and Yolo-Auto (vllm-0.29.x behind a billing proxy —
//                      verified live 2026-09-23: streams bare `reasoning`).
//
// ORDER MATTERS, AND SO DOES `nonempty`. Some OpenRouter passthroughs send an
// EMPTY `reasoning_content` alongside a POPULATED `reasoning`. Taking the
// first key that merely exists drops every reasoning token on those
// endpoints, silently — no error, just a model that appears to think in
// silence. `nonempty` is what makes the fallthrough happen.
inline const Lens<std::string>& reasoning_delta() {
    static const Lens<std::string> l =
          nonempty(key<std::string>("reasoning_content"))
        | nonempty(key<std::string>("reasoning"));
    return l;
}

// ── prose content ────────────────────────────────────────────────────────
//
// The ordinary case is a plain string. Mistral's reasoning models instead
// send an ARRAY of typed parts (probed live on mistral-small-latest with
// reasoning_effort=high):
//
//   {"content":[{"type":"thinking","thinking":[{"type":"text","text":"…"}]},
//               {"type":"text","text":"…"}]}
//
// Before this array form was handled it was silently DROPPED: no reasoning,
// no prose, and no liveness heartbeat during a long reasoning pass — which
// starved the stall watchdog and looked like a hang.
inline const Lens<std::string>& content_delta() {
    static const Lens<std::string> l = key<std::string>("content");
    return l;
}

// One element of the structured content-parts array, when it carries prose.
inline const Lens<std::string>& content_part_text() {
    static const Lens<std::string> l =
        when("type", "text", key<std::string>("text"));
    return l;
}

// One element of the structured content-parts array, when it carries
// thinking. Two shapes seen: nested (`thinking` is itself an array of text
// parts) and flat (`text` directly on the thinking part).
inline const Lens<std::vector<std::string>>& content_part_thinking_nested() {
    static const Lens<std::vector<std::string>> l =
        when("type", "thinking",
             collect("thinking", when("type", "text", key<std::string>("text"))));
    return l;
}
inline const Lens<std::string>& content_part_thinking_flat() {
    static const Lens<std::string> l =
        when("type", "thinking", key<std::string>("text"));
    return l;
}

// ── tool-call index ──────────────────────────────────────────────────────
//
// OpenAI sends `index` on each tool_calls delta so fragments can be
// reassembled across chunks. Many clones omit it and send one call per
// element, in order. Absent `index`, the ARRAY POSITION is the only identity
// available — which is why this returns an optional rather than defaulting to
// 0: silently folding every indexless call onto slot 0 concatenates two
// different calls' arguments into one malformed blob.
inline const Lens<int>& tool_call_index() {
    static const Lens<int> l = key<int>("index");
    return l;
}

// ── error envelopes ──────────────────────────────────────────────────────
//
// Three shapes seen for the same thing:
//
//   {"error":{"message":"…"}}   OpenAI, and everything that copied it
//                               (verified: Yolo-Auto returns exactly this
//                               for 401/403).
//   {"error":"…"}               some gateways flatten it to a bare string
//   {"message":"…","code":500}  llama.cpp's bare form, no `error` wrapper
//
// A stream that fails with an unrecognised envelope surfaces as "the model
// stopped for no reason", which is the worst possible diagnostic. All three
// belong in one place.
inline const Lens<std::string>& error_message() {
    static const Lens<std::string> l =
          under("error", key<std::string>("message"))
        | key<std::string>("error")
        | key<std::string>("message");
    return l;
}

// ── model metadata from /v1/models ───────────────────────────────────────
//
// NOT IN OPENAI'S SHAPE AT ALL. `context_length` / `max_model_len` are a vLLM
// extension, and they are the reason agentty can size a context window for a
// custom host without being told: verified on Yolo-Auto, which reports
// 131072 for qwen3.8-flash and honours it (a 125k-token prompt returned 200
// on a free key).
//
// Absent on OpenAI proper and on most hosted gateways — hence Observed<>
// at the call site rather than a defaulted int.
inline const Lens<int>& model_context_window() {
    static const Lens<int> l =
          key<int>("context_length")
        | key<int>("max_model_len")
        | key<int>("context_window");
    return l;
}

} // namespace agentty::provider::openai::dialect
