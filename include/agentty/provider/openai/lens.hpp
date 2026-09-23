#pragma once
// agentty/provider/openai/lens.hpp — observing a dialect that has no spec.
//
// ── WHY THIS IS NOT acp::Codec<T> ────────────────────────────────────────
//
// acp-cpp and mcp-cpp model their wire with `Codec<T>`, a partial ISOMORPHISM
//
//        T  ⇄  Json           decode ∘ encode = id_T
//      encode   decode
//
// and that law is meaningful because ACP and MCP are PROTOCOLS: a published
// specification says what the bytes are, so an unknown field is a peer bug and
// `throw CodecError` is the correct response.
//
// "OpenAI-compatible" is not a protocol. It is an API shape that won by
// adoption and was then cloned. There is no RFC, no registry, no version
// negotiation, and no body that owns it. The two de-facto sources of truth —
// OpenAI's own openapi document and vLLM's server — DISAGREE, and the
// ecosystem follows whichever is closer to hand. Concretely:
//
//   • OpenAI's Responses API streams thinking as `reasoning`.
//   • DeepSeek introduced `reasoning_content` on Chat Completions.
//   • vLLM followed DeepSeek. Some gateways emit BOTH, one of them empty.
//   • `context_length` in /v1/models is a vLLM extension that is not in
//     OpenAI's shape at all.
//
// None of those is a violation, because there is nothing to violate. So the
// algebra has to be different in two ways:
//
//   1. THERE IS NO ENCODE SIDE. agentty consumes provider responses; it never
//      produces one. A Codec needs an inverse and a spec on both sides; here
//      there is neither, so the round-trip law cannot even be stated.
//
//   2. FAILURE IS ORDINARY, NOT EXCEPTIONAL. A missing key means "this server
//      spells it differently", not "this server is broken". Decoding must
//      DEGRADE, never throw.
//
// ── WHAT THIS IS INSTEAD ─────────────────────────────────────────────────
//
// A `Lens<T>` witnesses a partial OBSERVATION of a Json value
//
//        observe : Json ⇀ T
//
// and the laws are about choice rather than round-tripping. `Lens` is an
// Alternative over Maybe; `|` is its choice operator:
//
//        (a | b) succeeds   ⟺   a succeeds ∨ b succeeds
//        (a | b) | c        =   a | (b | c)             (associative)
//        a | fail  =  a  =  fail | a                    (fail is the identity)
//
// which is exactly the structure of "this field has three spellings in the
// wild, try them in order of specificity". The deviations stop being `if`
// branches buried in a 3000-line decoder and become DATA — a declaration you
// can read, test against captured fixtures, and extend with one line.
//
// ── SSOT ─────────────────────────────────────────────────────────────────
//
// dialect.hpp holds the observation tables built from this algebra. They are
// the single source of truth for how a field is spelled on this wire; the
// transport observes THROUGH them and nowhere else.

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

namespace agentty::provider::openai {

using Json = nlohmann::json;

// ── Lens<T> ──────────────────────────────────────────────────────────────
// An erased partial observation. Total in the sense that it never throws:
// a shape it does not recognise yields nullopt, which the caller treats as
// "not present in this dialect", not as an error.
template <class T>
struct Lens {
    using Observe = std::function<std::optional<T>(const Json&)>;
    Observe observe;

    [[nodiscard]] std::optional<T> operator()(const Json& j) const {
        return observe ? observe(j) : std::nullopt;
    }
};

// ── fail : Lens<T> ───────────────────────────────────────────────────────
// The identity of `|`. Observes nothing, ever.
template <class T>
[[nodiscard]] inline Lens<T> fail() {
    return Lens<T>{[](const Json&) { return std::optional<T>{}; }};
}

// ── a | b : choice ───────────────────────────────────────────────────────
// Try `a`; if it observes nothing, try `b`. Associative, with `fail` as
// identity — the Alternative instance that makes a deviation table a single
// readable expression.
template <class T>
[[nodiscard]] inline Lens<T> operator|(Lens<T> a, Lens<T> b) {
    return Lens<T>{[a = std::move(a), b = std::move(b)](const Json& j)
                       -> std::optional<T> {
        if (auto v = a(j)) return v;
        return b(j);
    }};
}

// ── key : Lens<T> ────────────────────────────────────────────────────────
// Observe one object member, when it is present AND of the right JSON type.
// A wrong-typed member yields nullopt rather than throwing: a gateway that
// sends `reasoning: null` alongside a populated `reasoning_content` must fall
// through to the next alternative, not abort the stream.
template <class T>
[[nodiscard]] inline Lens<T> key(std::string name) {
    return Lens<T>{[name = std::move(name)](const Json& j) -> std::optional<T> {
        if (!j.is_object()) return std::nullopt;
        const auto it = j.find(name);
        if (it == j.end()) return std::nullopt;
        if constexpr (std::is_same_v<T, std::string>) {
            if (!it->is_string()) return std::nullopt;
            return it->template get<std::string>();
        } else if constexpr (std::is_integral_v<T>) {
            if (!it->is_number_integer()) return std::nullopt;
            return it->template get<T>();
        } else {
            if (it->is_null()) return std::nullopt;
            return it->template get<T>();
        }
    }};
}

// ── nonempty : Lens<string> → Lens<string> ───────────────────────────────
// Reject an empty observation so it falls through to the next alternative.
//
// THIS ONE IS LOAD-BEARING, not a nicety. Some OpenRouter passthroughs send an
// EMPTY `reasoning_content` alongside a populated `reasoning`. Taking the
// first key that merely EXISTS silently drops every token of reasoning on
// those endpoints. The bug is invisible: no error, no warning, just a model
// that appears to think in silence.
[[nodiscard]] inline Lens<std::string> nonempty(Lens<std::string> inner) {
    return Lens<std::string>{
        [inner = std::move(inner)](const Json& j) -> std::optional<std::string> {
            auto v = inner(j);
            if (v && !v->empty()) return v;
            return std::nullopt;
        }};
}

// ── under : (key, Lens<T>) → Lens<T> ─────────────────────────────────────
// Descend into a nested object and observe there. Lets a table say
// `under("delta", reasoning)` without the caller unpacking the envelope.
template <class T>
[[nodiscard]] inline Lens<T> under(std::string name, Lens<T> inner) {
    return Lens<T>{[name = std::move(name), inner = std::move(inner)](
                       const Json& j) -> std::optional<T> {
        if (!j.is_object()) return std::nullopt;
        const auto it = j.find(name);
        if (it == j.end() || !it->is_object()) return std::nullopt;
        return inner(*it);
    }};
}

// ── map : (Lens<A>, A → B) → Lens<B> ─────────────────────────────────────
// The functor instance. Observe an A, then transform it.
template <class A, class F>
[[nodiscard]] inline auto map(Lens<A> a, F f)
    -> Lens<std::invoke_result_t<F, A>> {
    using B = std::invoke_result_t<F, A>;
    return Lens<B>{[a = std::move(a), f = std::move(f)](
                       const Json& j) -> std::optional<B> {
        if (auto v = a(j)) return f(*v);
        return std::nullopt;
    }};
}

// ── collect : Lens<vector<T>> ────────────────────────────────────────────
// Observe an ARRAY member by walking each element with an element lens and
// keeping the ones that observe. Used for the structured content-parts form
// (Mistral) where `content` is a list of typed parts instead of a string.
template <class T>
[[nodiscard]] inline Lens<std::vector<T>> collect(std::string name,
                                                  Lens<T> element) {
    return Lens<std::vector<T>>{
        [name = std::move(name), element = std::move(element)](
            const Json& j) -> std::optional<std::vector<T>> {
            if (!j.is_object()) return std::nullopt;
            const auto it = j.find(name);
            if (it == j.end() || !it->is_array()) return std::nullopt;
            std::vector<T> out;
            out.reserve(it->size());
            for (const auto& el : *it)
                if (auto v = element(el)) out.push_back(std::move(*v));
            if (out.empty()) return std::nullopt;
            return out;
        }};
}

// ── when : (predicate, Lens<T>) → Lens<T> ────────────────────────────────
// Gate an observation on a discriminator, e.g. only read `text` out of a
// content part whose `type` is "thinking".
template <class T>
[[nodiscard]] inline Lens<T> when(std::string tag_key, std::string tag_value,
                                  Lens<T> inner) {
    return Lens<T>{[tag_key = std::move(tag_key), tag_value = std::move(tag_value),
                    inner = std::move(inner)](const Json& j) -> std::optional<T> {
        if (!j.is_object()) return std::nullopt;
        const auto it = j.find(tag_key);
        if (it == j.end() || !it->is_string()) return std::nullopt;
        if (it->template get_ref<const std::string&>() != tag_value)
            return std::nullopt;
        return inner(j);
    }};
}

// ── Provenance / Observed<T> ─────────────────────────────────────────────
//
// A fact about an endpoint is one of three things, and "unknown" MUST NOT
// collapse into "no".
//
// The bug this exists to prevent has a name and a number. PR #53 proposed a
// registry row for a provider, DECLARING that it served models `yolo` and
// `yolo-small`. Probed with a real key, `/v1/models` listed neither, and
// calling them returned 403 for every free account. A declaration is not an
// observation. In an ecosystem with no source of truth, only the probe can
// speak — and the type system should refuse to let a hint impersonate a fact.
enum class Provenance : std::uint8_t {
    Probed,    // we dialled the endpoint and saw this. authoritative.
    Declared,  // a registry row or a doc page asserts it. a HINT.
    Absent,    // we probed, and it is not there.
};

template <class T>
struct Observed {
    Provenance       how = Provenance::Absent;
    std::optional<T> value{};

    [[nodiscard]] static Observed probed(T v) {
        return {Provenance::Probed, std::move(v)};
    }
    [[nodiscard]] static Observed declared(T v) {
        return {Provenance::Declared, std::move(v)};
    }
    [[nodiscard]] static Observed absent() { return {Provenance::Absent, {}}; }

    // DELIBERATELY NOT operator bool, and deliberately not named `has_value`.
    // `if (caps.tools)` would read a Declared hint as true, which is exactly
    // the confusion this type exists to make unrepresentable. A caller must
    // say which standard of evidence it needs.
    [[nodiscard]] bool confirmed() const noexcept {
        return how == Provenance::Probed && value.has_value();
    }
    // Good enough when being wrong is cheap (picker copy, a default), not
    // when it decides whether a request is well-formed.
    [[nodiscard]] bool believed() const noexcept {
        return how != Provenance::Absent && value.has_value();
    }
    [[nodiscard]] const T& or_else(const T& fallback) const noexcept {
        return value ? *value : fallback;
    }
};

} // namespace agentty::provider::openai
