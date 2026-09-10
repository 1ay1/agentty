// fold.cpp — one message → Facts. The ONLY place anything is counted.
//
// Every statistic in the panel is a line in fold(). That is the DRY claim
// the design rests on: adding "how many tokens did subagents burn" is a
// field on Facts and a line here, not a second walk of the transcript with
// its own cache and its own idea of what a turn is.

#include "agentty/domain/stats/facts.hpp"

#include <chrono>

namespace agentty::stats {
namespace {

// Everything before the first '-' or '/' — "claude-sonnet-4-6" → "claude",
// "openai/gpt-5" → "openai". A hint, not an authority: the wire id is the
// only provenance a turn carries, and asking the registry here would
// couple the fold to live model state that a reloaded thread may not have.
[[nodiscard]] std::string_view provider_hint(std::string_view id) {
    const auto cut = id.find_first_of("-/");
    return cut == std::string_view::npos ? id : id.substr(0, cut);
}

void push_capped(std::vector<double>& v, double x) {
    if (v.size() >= Facts::kSeriesCap) v.erase(v.begin());
    v.push_back(x);
}

}  // namespace

void Facts::fold(const Message& msg) {
    if (msg.role == Role::User) {
        ++session.user_turns;
        if (msg.proactive) {
            ++retrieval.injections;
            if (msg.proactive->confidence) {
                ++retrieval.with_confidence;
                retrieval.confidence_sum += *msg.proactive->confidence;
            }
        }
        return;
    }
    if (msg.role != Role::Assistant) return;

    // Smart Mode's own routing messages are bookkeeping, not turns. Counting
    // them would inflate every denominator on the panel.
    if (msg.smart_routing) return;

    ++session.assistant_turns;
    if (msg.error)            ++session.errors;
    if (msg.is_compact_summary) ++session.compact_summaries;

    // ── Models ──────────────────────────────────────────────────────────
    if (!msg.served_model.empty()) {
        models.by_model.add(msg.served_model.value);
        models.by_provider_hint.add(provider_hint(msg.served_model.value));
    }

    // ── Smart Mode ──────────────────────────────────────────────────────
    //
    // The bucket split is load-bearing. A turn with no served_role ran with
    // Smart Mode OFF; counting it as Strategic would overstate the
    // flagship's share, and dropping it would understate the denominator.
    // `delegated` is over ROUTED turns only, which is what makes it a
    // verdict on the feature rather than a description of the session.
    if (msg.served_role) {
        ++smart.routed;
        smart.by_role.add(smart::role_display_name(*msg.served_role));
        if (!msg.served_model.empty()) smart.by_model.add(msg.served_model.value);
        if (*msg.served_role != smart::ModelRole::Strategic) ++smart.delegated;
    } else {
        ++smart.unrouted;
    }

    // ── Reasoning ───────────────────────────────────────────────────────
    if (msg.reasoning_ms > 0 || !msg.thinking_blocks.empty()
        || !msg.thinking.empty()) {
        ++reasoning.turns;
        if (msg.reasoning_ms > 0)
            reasoning.ms += static_cast<std::uint64_t>(msg.reasoning_ms);
        reasoning.blocks += msg.thinking_blocks.size();
    }

    // ── Tools ───────────────────────────────────────────────────────────
    for (const auto& tc : msg.tool_calls) {
        ++tools.total;
        tools.by_name.add(tc.name.value);
        // The status names are the user's vocabulary, not the variant's:
        // these appear verbatim as row labels.
        const char* st = std::visit([](const auto& s) -> const char* {
            using T = std::decay_t<decltype(s)>;
            if constexpr (std::is_same_v<T, ToolUse::Done>)     return "done";
            else if constexpr (std::is_same_v<T, ToolUse::Failed>)   return "failed";
            else if constexpr (std::is_same_v<T, ToolUse::Rejected>) return "rejected";
            else return "pending";
        }, tc.status);
        tools.by_status.add(st);
        // Only settled calls have a duration. A running call's elapsed time
        // is a live number, and folding it would bake "how long had it been
        // going when I opened the panel" into a sealed statistic.
        const auto started  = tc.started_at();
        const auto finished = tc.finished_at();
        if (started.time_since_epoch().count() != 0
            && finished.time_since_epoch().count() != 0
            && finished > started) {
            const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                finished - started).count();
            tools.latency.add(static_cast<std::uint32_t>(ms < 0 ? 0 : ms));
        }
    }

    // ── Everything from the sealed telemetry ────────────────────────────
    //
    // Absent means "not measured", which is different from measured zero —
    // so an unmeasured turn is excluded from measured_turns and therefore
    // from every average, rather than dragging them toward zero.
    if (!msg.telemetry) return;
    const auto& t = *msg.telemetry;
    ++session.measured_turns;
    session.wall_ms += static_cast<std::uint64_t>(t.ttft_ms)
                     + static_cast<std::uint64_t>(t.stream_ms);

    tokens.input          += t.input_tokens;
    tokens.output         += t.output_tokens;
    tokens.reasoning      += t.reasoning_tokens;
    tokens.cache_read     += t.cache_read;
    tokens.cache_creation += t.cache_creation;
    reasoning.tokens      += t.reasoning_tokens;
    if (t.output_tokens) {
        tokens.per_turn_output.add(t.output_tokens);
        push_capped(tokens.output_series, static_cast<double>(t.output_tokens));
    }

    // Cache: the three-way split a band renders. `input_tokens` excludes
    // the cache fields on the wire, so it IS the miss term — no arithmetic
    // needed, and none invented.
    if (t.cache_read || t.cache_creation) ++cache.turns_with_cache;
    cache.hits   += t.cache_read;
    cache.writes += t.cache_creation;
    cache.misses += t.input_tokens;
    if (const double r = t.cache_hit_ratio(); r >= 0.0)
        push_capped(cache.ratio_series, r);

    // Context: the true prefix is input + both cache terms.
    const std::uint64_t prefix = static_cast<std::uint64_t>(t.input_tokens)
                               + t.cache_read + t.cache_creation;
    if (prefix > context.peak_input) context.peak_input = prefix;
    if (prefix) push_capped(context.prefix_series, static_cast<double>(prefix));

    // Stream health.
    if (t.degraded()) ++stream.degraded_turns;
    stream.transient  += t.transient_retries;
    stream.mid_stream += t.mid_stream_failures;
    stream.stalls     += t.no_progress_failures;
    stream.wire_bytes += t.wire_bytes;
    if (t.ttft_ms)   stream.ttft.add(t.ttft_ms);
    if (t.stream_ms) stream.stream_ms.add(t.stream_ms);
    if (t.ttft_ms)   push_capped(stream.ttft_series,
                                 static_cast<double>(t.ttft_ms));
    // Output tokens per second of GENERATION time, not of wall time:
    // including the wait would fold the provider's queueing into a number
    // that is supposed to be about how fast it writes.
    if (t.stream_ms && t.output_tokens)
        push_capped(stream.rate_series,
                    static_cast<double>(t.output_tokens)
                      / (static_cast<double>(t.stream_ms) / 1000.0));
}

void Facts::merge(const Facts& o) {
    session.user_turns        += o.session.user_turns;
    session.assistant_turns   += o.session.assistant_turns;
    session.errors            += o.session.errors;
    session.compact_summaries += o.session.compact_summaries;
    session.measured_turns    += o.session.measured_turns;
    session.wall_ms           += o.session.wall_ms;

    models.by_model.merge(o.models.by_model);
    models.by_provider_hint.merge(o.models.by_provider_hint);

    smart.routed    += o.smart.routed;
    smart.unrouted  += o.smart.unrouted;
    smart.delegated += o.smart.delegated;
    smart.by_role.merge(o.smart.by_role);
    smart.by_model.merge(o.smart.by_model);

    tokens.input          += o.tokens.input;
    tokens.output         += o.tokens.output;
    tokens.reasoning      += o.tokens.reasoning;
    tokens.cache_read     += o.tokens.cache_read;
    tokens.cache_creation += o.tokens.cache_creation;
    tokens.per_turn_output.merge(o.tokens.per_turn_output);
    for (double x : o.tokens.output_series) push_capped(tokens.output_series, x);

    cache.turns_with_cache += o.cache.turns_with_cache;
    cache.hits   += o.cache.hits;
    cache.writes += o.cache.writes;
    cache.misses += o.cache.misses;
    for (double x : o.cache.ratio_series) push_capped(cache.ratio_series, x);

    tools.total += o.tools.total;
    tools.by_name.merge(o.tools.by_name);
    tools.by_status.merge(o.tools.by_status);
    tools.latency.merge(o.tools.latency);

    reasoning.turns  += o.reasoning.turns;
    reasoning.ms     += o.reasoning.ms;
    reasoning.tokens += o.reasoning.tokens;
    reasoning.blocks += o.reasoning.blocks;

    stream.degraded_turns += o.stream.degraded_turns;
    stream.transient      += o.stream.transient;
    stream.mid_stream     += o.stream.mid_stream;
    stream.stalls         += o.stream.stalls;
    stream.wire_bytes     += o.stream.wire_bytes;
    stream.ttft.merge(o.stream.ttft);
    stream.stream_ms.merge(o.stream.stream_ms);
    for (double x : o.stream.ttft_series) push_capped(stream.ttft_series, x);
    for (double x : o.stream.rate_series) push_capped(stream.rate_series, x);

    context.compactions += o.context.compactions;
    if (o.context.peak_input > context.peak_input)
        context.peak_input = o.context.peak_input;
    for (double x : o.context.prefix_series) push_capped(context.prefix_series, x);

    retrieval.injections      += o.retrieval.injections;
    retrieval.with_confidence += o.retrieval.with_confidence;
    retrieval.confidence_sum  += o.retrieval.confidence_sum;
}

const Facts& Projection::refresh(const Thread& t) {
    const Epoch next = Epoch::of(t);

    // Fork, rewind and compaction all mutate history rather than appending.
    // Detect by value, rebuild wholesale — see the header note on why this
    // deliberately does not try to be clever.
    if (!epoch_.extends_to(next)) {
        sealed_.clear();
        consumed_ = 0;
    }
    epoch_ = next;

    // The LAST message is always volatile: it is the one being streamed
    // into. Seal everything strictly before it.
    const std::size_t seal_to = t.messages.empty() ? 0 : t.messages.size() - 1;
    for (; consumed_ < seal_to; ++consumed_)
        sealed_.fold(t.messages[consumed_]);

    // Compaction count is a thread-level fact, not a per-message one.
    view_ = sealed_;
    view_.context.compactions = t.compactions.size();

    if (!t.messages.empty()) {
        Facts tail;
        tail.fold(t.messages.back());
        view_.merge(tail);
    }
    return view_;
}

}  // namespace agentty::stats
