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
// NOT IN OPENAI'S SHAPE AT ALL. OpenAI's own /v1/models returns only
// {id, object, created, owned_by} — no window. So every gateway that wanted
// to publish one invented its own place to put it, and there are now ~15
// spellings across nested blocks (`top_provider`, `model_info`, `meta`) and
// GGUF arch-prefixed keys ("qwen2.context_length").
//
// THE FULL LADDER LIVES IN detail::advertised_window_tokens()
// (src/provider/openai/transport.cpp) and that function is the SSOT — it is
// the one place that knows all of them, in priority order, with each entry
// attributed to the vendor it was observed on. Do not duplicate it here: a
// second partial list is worse than none, because a spelling this one misses
// silently falls back to a default window, the gauge misreads, and
// auto-compaction fires at the wrong point (that is issue #49).
//
// This lens is deliberately the NARROW case: the two flat keys a probe sees
// on a plain vLLM row, used where only a quick top-level read is wanted.
// Anything that needs the real answer calls advertised_window_tokens().
//
// Verified on Yolo-Auto (vllm-0.29.1, 2026-09-23): both keys present, 131072,
// and honoured — a 125,040-token prompt returned 200 on a free key.
inline const Lens<int>& model_context_window_flat() {
    static const Lens<int> l =
          key<int>("context_length")
        | key<int>("max_model_len");
    return l;
}

// ── declared reasoning support, from a /models row ───────────────────────
//
// Whether a model CAN reason, as the server itself declares it — distinct
// from reasoning_delta() above, which reads the thinking TEXT off a stream.
// This is the fact that decides whether agentty offers an effort ladder and
// whether it sends `reasoning_effort` at all.
//
// It matters most for LOCAL servers. resolved_caps() resolves capability as
//
//     per-model override  >  env  >  live catalog  >  id inference
//
// and the id-inference rung is a heuristic over hosted naming conventions
// (`:7b` tags, known family prefixes). A llama.cpp server reports GGUF
// filenames and an LM Studio server reports `publisher/model` keys, so that
// rung is close to useless there — which is exactly why the catalog rung
// has to be filled from what the server says.
//
// Two spellings observed:
//
//   "reasoning": true                    Mistral's /v1/models. A plain bool.
//   "reasoning": {"allowed_options":     LM Studio's /api/v1/models
//                   ["off","on"],        (documented in its REST reference;
//                 "default":"on"}        a model with NO reasoning support
//                                        omits the key entirely).
//
// The object form is why LM Studio silently got no reasoning: the reader
// tested is_boolean() and skipped anything else, so every LM Studio model
// fell through to id inference and was classified by its filename.
//
// Presence of the object IS the declaration — a model that cannot reason
// has no `reasoning` key at all, so `allowed_options` never needs parsing
// to answer the yes/no question. (What the ladder should be is a separate
// question; see declared_reasoning_default below.)
inline const Lens<bool>& declared_reasoning() {
    static const Lens<bool> l =
          key<bool>("reasoning")                       // Mistral: plain bool
        | truthy_object("reasoning");                  // LM Studio: object
    return l;
}

// Whether a model that CAN reason has it on by default.
//
// LM Studio's object carries `"default": "on" | "off"`. A model whose
// default is "off" still supports reasoning — it just needs asking — so
// this is deliberately separate from declared_reasoning() rather than
// folded into it.
inline const Lens<std::string>& declared_reasoning_default() {
    static const Lens<std::string> l =
        under("reasoning", key<std::string>("default"));
    return l;
}

// ── declared tool support, from a /models row ────────────────────────────
//
// LM Studio's capability object again (`trained_for_tool_use`). Named for
// what it is: the model was TRAINED for tool use, which is a stronger claim
// than "the server will accept a tools array" — llama.cpp will accept one
// for any model and let it fail at generation time.
inline const Lens<bool>& declared_tool_use() {
    static const Lens<bool> l = key<bool>("trained_for_tool_use");
    return l;
}

// ── declared capability, from llama.cpp /props ───────────────────────────
//
// llama.cpp has no capability object on /v1/models rows, so the two lenses
// above never fire there. But it does answer /props — which agentty already
// fetches for the runtime context window — and that response carries
// `chat_template_caps`.
//
// Those caps are not metadata somebody typed in. llama.cpp RUNS the model's
// own jinja chat template against probe inputs at load time and diffs the
// rendered output to see what the template actually reacts to (see
// common/jinja/caps.cpp, caps_get). So it is a measurement of the exact
// template this server will apply to our request — strictly better evidence
// than any filename or third-party catalog, and it self-corrects when the
// user swaps the GGUF.
//
// The key names are fixed by caps::to_map() in that same file:
//
//   supports_tools                the template renders a tools array
//   supports_tool_calls           ... and assistant tool_calls back
//   supports_parallel_tool_calls
//   supports_system_role
//   supports_string_content / supports_typed_content
//   supports_object_arguments
//   supports_preserve_reasoning   keeps thinking across turns, not just last
//   supports_reasoning_effort     the template honours a reasoning_effort kwarg
//
// Only the last one answers "should agentty show an effort ladder". A server
// whose template ignores reasoning_effort will happily accept the field and
// drop it on the floor, which is the worst outcome: the picker offers a dial
// that is wired to nothing.
inline const Lens<bool>& template_reasoning_effort() {
    static const Lens<bool> l =
        under("chat_template_caps", key<bool>("supports_reasoning_effort"));
    return l;
}

// Whether the template can render tool calls at all.
//
// Read supports_tool_calls, not supports_tools: a template that renders the
// tools array but cannot render the assistant's tool_calls back into history
// breaks on turn two, which looks like the model "forgetting" it called
// anything. Both default to true in llama.cpp's caps struct, so a false here
// is a template that demonstrably failed the probe.
inline const Lens<bool>& template_tool_calls() {
    static const Lens<bool> l =
        under("chat_template_caps", key<bool>("supports_tool_calls"));
    return l;
}

} // namespace agentty::provider::openai::dialect
