// extract.cpp — Facts → Metrics. One function per section, nothing else.
//
// Every function here is a projection: it reads counters and writes rows.
// No counting happens in this file (that is fold.cpp) and no formatting
// (that is unit.hpp) — which is what lets a new statistic be a fold line
// plus a row here, with the view untouched.

#include "agentty/domain/stats/tabs.hpp"

namespace agentty::stats {
namespace {

// Hue slots. An int on Metric::Part rather than a maya::Color, because
// domain/ does not depend on the renderer — the panel maps these onto the
// theme. Named so a reader of an extractor can see what the band means.
enum Hue { kNeutral = 0, kGood = 1, kWarn = 2, kBad = 3, kAccent = 4 };

void ranked_rows(const Tally& t, std::vector<Metric>& out, Unit u = Unit::Count) {
    const double total = static_cast<double>(t.total());
    for (const auto& [label, n] : t.ranked())
        out.push_back(Metric{.label = label,
                             .unit  = u,
                             .value = static_cast<double>(n),
                             .of    = total,
                             .detail = format(Unit::Ratio,
                                              total > 0 ? n / total : 0.0)});
}

void kv(std::vector<Metric>& out, std::string label, Unit u, double v,
        std::string detail = {}) {
    out.push_back(Metric{.label = std::move(label), .unit = u, .value = v,
                         .of = 0, .detail = std::move(detail)});
}

// ── Session ─────────────────────────────────────────────────────────────

void session_hero(const Facts& f, std::vector<Metric>& out) {
    const auto turns = f.session.assistant_turns;
    // The hero is a SENTENCE, not a row: figure first, then the clause
    // that says what it means. "5 turns" alone makes the reader look for
    // the verdict; "— no errors" is the verdict.
    std::string caption;
    if (f.session.errors)
        caption = "\xe2\x80\x94 " + std::to_string(f.session.errors)
                + (f.session.errors == 1 ? " ended in an error"
                                         : " ended in errors");
    else if (turns)
        caption = "\xe2\x80\x94 none ended in an error";
    else
        caption = "\xe2\x80\x94 nothing has run in this thread yet";
    out.push_back(Metric{
        .label = turns == 1 ? "1 turn" : std::to_string(turns) + " turns",
        .unit  = Unit::Count,
        .value = static_cast<double>(turns),
        .detail = std::move(caption)});
}

void session_counts(const Facts& f, std::vector<Metric>& out) {
    kv(out, "You asked",       Unit::Count, static_cast<double>(f.session.user_turns));
    kv(out, "Agent replied",   Unit::Count, static_cast<double>(f.session.assistant_turns));
    if (f.session.errors)
        kv(out, "Errors",      Unit::Count, static_cast<double>(f.session.errors));
    if (f.tools.total)
        kv(out, "Tool calls",  Unit::Count, static_cast<double>(f.tools.total));
    if (f.session.compact_summaries)
        kv(out, "Compactions", Unit::Count,
           static_cast<double>(f.session.compact_summaries));
}

void session_time(const Facts& f, std::vector<Metric>& out) {
    if (!f.session.measured_turns) return;
    kv(out, "Time on the wire", Unit::Millis,
       static_cast<double>(f.session.wall_ms));
    kv(out, "Average turn",     Unit::Millis,
       static_cast<double>(f.session.wall_ms)
         / static_cast<double>(f.session.measured_turns));
    if (f.session.measured_turns < f.session.assistant_turns)
        kv(out, "Measured turns", Unit::Count,
           static_cast<double>(f.session.measured_turns),
           "older turns predate telemetry");
}

// ── Models ──────────────────────────────────────────────────────────────

void models_by_model(const Facts& f, std::vector<Metric>& out) {
    ranked_rows(f.models.by_model, out);
}
void models_by_provider(const Facts& f, std::vector<Metric>& out) {
    // Only worth a section when more than one provider actually served —
    // otherwise it restates the section above with fewer rows.
    if (f.models.by_provider_hint.size() < 2) return;
    ranked_rows(f.models.by_provider_hint, out);
}

// ── Smart Mode ──────────────────────────────────────────────────────────

void smart_hero(const Facts& f, std::vector<Metric>& out) {
    const double share = f.smart.routed
        ? static_cast<double>(f.smart.delegated) / static_cast<double>(f.smart.routed)
        : 0.0;
    // The denominator is part of the sentence. "62%" of WHAT is the first
    // question a share invites, and a hero that does not answer it sends
    // the reader to the table below to reconstruct it.
    out.push_back(Metric{
        .label = format(Unit::Ratio, share),
        .unit  = Unit::Ratio,
        .value = share,
        .detail = "of " + std::to_string(f.smart.routed)
                + " routed turns ran below the Strategic model"});
}

void smart_by_role(const Facts& f, std::vector<Metric>& out) {
    ranked_rows(f.smart.by_role, out);
    if (f.smart.unrouted)
        kv(out, "Smart Mode off", Unit::Count,
           static_cast<double>(f.smart.unrouted),
           "not counted in the share above");
}

void smart_by_model(const Facts& f, std::vector<Metric>& out) {
    ranked_rows(f.smart.by_model, out);
}

// ── Tokens ──────────────────────────────────────────────────────────────

void tokens_totals(const Facts& f, std::vector<Metric>& out) {
    const double total = static_cast<double>(f.tokens.input + f.tokens.output);
    kv(out, "In",  Unit::Tokens, static_cast<double>(f.tokens.input));
    kv(out, "Out", Unit::Tokens, static_cast<double>(f.tokens.output));
    if (f.tokens.reasoning)
        kv(out, "…of which reasoning", Unit::Tokens,
           static_cast<double>(f.tokens.reasoning),
           format(Unit::Ratio,
                  f.tokens.output ? static_cast<double>(f.tokens.reasoning)
                                      / static_cast<double>(f.tokens.output)
                                  : 0.0));
    kv(out, "Total", Unit::Tokens, total);
    // Per-turn figures live in the SAME section as the totals: they share
    // the unit, so they share the bar's scale, and seeing "largest turn"
    // against "total" is the comparison that says whether one turn
    // dominated the thread. Split across two sections each got its own
    // scale and the relationship vanished.
    const auto& h = f.tokens.per_turn_output;
    if (h.count()) {
        kv(out, "Average turn", Unit::Tokens, h.mean());
        kv(out, "Largest turn", Unit::Tokens, static_cast<double>(h.max()));
    }
}

void tokens_plot(const Facts& f, std::vector<Metric>& out) {
    if (f.tokens.output_series.size() < 2) return;
    out.push_back(Metric{.label  = "output tokens per turn",
                         .unit   = Unit::Tokens,
                         .series = f.tokens.output_series});
}

// ── Cache ───────────────────────────────────────────────────────────────

void cache_band(const Facts& f, std::vector<Metric>& out) {
    const double total = static_cast<double>(f.cache.hits + f.cache.writes
                                           + f.cache.misses);
    if (total <= 0) return;
    Metric m;
    m.label = "input tokens by origin";
    m.unit  = Unit::Tokens;
    m.value = total;
    m.parts = {
        {"cache hit " + format(Unit::Tokens, static_cast<double>(f.cache.hits)),
         static_cast<double>(f.cache.hits),   kGood},
        {"written "   + format(Unit::Tokens, static_cast<double>(f.cache.writes)),
         static_cast<double>(f.cache.writes), kWarn},
        {"uncached "  + format(Unit::Tokens, static_cast<double>(f.cache.misses)),
         static_cast<double>(f.cache.misses), kBad},
    };
    out.push_back(std::move(m));
}

void cache_rates(const Facts& f, std::vector<Metric>& out) {
    const double prefix = static_cast<double>(f.cache.hits + f.cache.writes
                                            + f.cache.misses);
    if (prefix <= 0) return;
    kv(out, "Hit rate", Unit::Ratio, static_cast<double>(f.cache.hits) / prefix,
       "of every prefix byte sent");
    kv(out, "Turns using cache", Unit::Count,
       static_cast<double>(f.cache.turns_with_cache));
    // The number this tab exists for: cache reads are ~10% the price of
    // fresh input on every provider that offers them, so this is the
    // single biggest lever on a long thread's cost.
    kv(out, "Tokens served from cache", Unit::Tokens,
       static_cast<double>(f.cache.hits));
}

// ── Tools ───────────────────────────────────────────────────────────────

void tools_by_name(const Facts& f, std::vector<Metric>& out) {
    ranked_rows(f.tools.by_name, out);
}

void tools_status(const Facts& f, std::vector<Metric>& out) {
    const double total = static_cast<double>(f.tools.by_status.total());
    if (total <= 0) return;
    Metric m;
    m.label = "calls by outcome";
    m.unit  = Unit::Count;
    m.value = total;
    for (const auto& [label, n] : f.tools.by_status.rows()) {
        const int hue = label == "done"     ? kGood
                      : label == "failed"   ? kBad
                      : label == "rejected" ? kWarn
                                            : kNeutral;
        m.parts.push_back({label + " " + format(Unit::Count,
                                                static_cast<double>(n)),
                           static_cast<double>(n), hue});
    }
    out.push_back(std::move(m));
}

void tools_latency(const Facts& f, std::vector<Metric>& out) {
    const auto& h = f.tools.latency;
    if (!h.count()) return;
    kv(out, "Median", Unit::Millis, h.quantile(0.5));
    kv(out, "p95",    Unit::Millis, h.quantile(0.95));
    kv(out, "Slowest", Unit::Millis, static_cast<double>(h.max()));
    kv(out, "Total",  Unit::Millis, static_cast<double>(h.sum()));
}

// ── Reasoning ───────────────────────────────────────────────────────────

void reasoning_rows(const Facts& f, std::vector<Metric>& out) {
    kv(out, "Turns that thought", Unit::Count,
       static_cast<double>(f.reasoning.turns),
       format(Unit::Ratio,
              f.session.assistant_turns
                ? static_cast<double>(f.reasoning.turns)
                    / static_cast<double>(f.session.assistant_turns)
                : 0.0));
    if (f.reasoning.ms)
        kv(out, "Time thinking", Unit::Millis,
           static_cast<double>(f.reasoning.ms));
    if (f.reasoning.tokens)
        kv(out, "Reasoning tokens", Unit::Tokens,
           static_cast<double>(f.reasoning.tokens));
    if (f.reasoning.turns && f.reasoning.ms)
        kv(out, "Average per turn", Unit::Millis,
           static_cast<double>(f.reasoning.ms)
             / static_cast<double>(f.reasoning.turns));
}

// ── Stream ──────────────────────────────────────────────────────────────

void stream_health(const Facts& f, std::vector<Metric>& out) {
    // Lead with the verdict. A list of zeroes is the answer "everything is
    // fine" spelled in a way that makes the reader work it out.
    const bool clean = f.stream.degraded_turns == 0;
    kv(out, "Turns that hit trouble", Unit::Count,
       static_cast<double>(f.stream.degraded_turns),
       clean ? "clean session" : "");
    if (f.stream.transient)
        kv(out, "Retries",        Unit::Count, static_cast<double>(f.stream.transient));
    if (f.stream.mid_stream)
        kv(out, "Mid-stream drops", Unit::Count, static_cast<double>(f.stream.mid_stream));
    if (f.stream.stalls)
        kv(out, "Stalls",         Unit::Count, static_cast<double>(f.stream.stalls));
}

void stream_timing(const Facts& f, std::vector<Metric>& out) {
    // TTFT and generation time answer different questions — one is "did the
    // provider make me wait", the other "was generation slow" — and they
    // have different fixes, so they are never summed. They share a UNIT
    // though, which is what lets the panel scale them on one bar and make
    // the split visible at a glance.
    if (f.stream.ttft.count()) {
        kv(out, "First byte",     Unit::Millis, f.stream.ttft.mean());
        kv(out, "First byte p95", Unit::Millis, f.stream.ttft.quantile(0.95));
    }
    if (f.stream.stream_ms.count()) {
        kv(out, "Generation",     Unit::Millis, f.stream.stream_ms.mean());
        kv(out, "Generation p95", Unit::Millis, f.stream.stream_ms.quantile(0.95));
    }
}

void stream_throughput(const Facts& f, std::vector<Metric>& out) {
    // Its own section because it is a RATE: on the timing bar above it
    // would be a bytes-per-second figure scaled against milliseconds,
    // which is a comparison that means nothing.
    const double secs = static_cast<double>(f.stream.stream_ms.sum()) / 1000.0;
    if (secs <= 0 || !f.stream.wire_bytes) return;
    kv(out, "Bytes delivered", Unit::Bytes,
       static_cast<double>(f.stream.wire_bytes));
    kv(out, "Throughput", Unit::Rate,
       static_cast<double>(f.stream.wire_bytes) / secs);
}

// ── Context ─────────────────────────────────────────────────────────────

void context_rows(const Facts& f, std::vector<Metric>& out) {
    kv(out, "Largest prefix", Unit::Tokens,
       static_cast<double>(f.context.peak_input));
    if (f.context.compactions)
        kv(out, "Compactions", Unit::Count,
           static_cast<double>(f.context.compactions),
           "history summarised to fit");
}

void context_plot(const Facts& f, std::vector<Metric>& out) {
    if (f.context.prefix_series.size() < 2) return;
    out.push_back(Metric{.label  = "prefix tokens per turn",
                         .unit   = Unit::Tokens,
                         .series = f.context.prefix_series});
}

// ── Retrieval ───────────────────────────────────────────────────────────

void retrieval_rows(const Facts& f, std::vector<Metric>& out) {
    kv(out, "Context injections", Unit::Count,
       static_cast<double>(f.retrieval.injections));
    if (f.retrieval.with_confidence)
        kv(out, "Average confidence", Unit::Ratio,
           f.retrieval.confidence_sum
             / static_cast<double>(f.retrieval.with_confidence));
}

}  // namespace

namespace sections {

const std::array<Section, 3> session{{
    {"",          Viz::Hero, &session_hero},
    {"Activity",  Viz::Kv,   &session_counts},
    {"Time",      Viz::Kv,   &session_time},
}};

const std::array<Section, 2> models{{
    {"By model",    Viz::Bars, &models_by_model},
    {"By provider", Viz::Bars, &models_by_provider},
}};

const std::array<Section, 3> smart{{
    {"",         Viz::Hero, &smart_hero},
    {"By role",  Viz::Bars, &smart_by_role},
    {"By model", Viz::Bars, &smart_by_model},
}};

const std::array<Section, 2> tokens{{
    {"Totals", Viz::Kv,   &tokens_totals},
    {"Trend",  Viz::Plot, &tokens_plot},
}};

const std::array<Section, 2> cache{{
    {"",      Viz::Band, &cache_band},
    {"Rates", Viz::Kv,   &cache_rates},
}};

const std::array<Section, 3> tools{{
    {"",         Viz::Band, &tools_status},
    {"By tool",  Viz::Bars, &tools_by_name},
    {"Latency",  Viz::Kv,   &tools_latency},
}};

const std::array<Section, 1> reasoning{{
    {"", Viz::Kv, &reasoning_rows},
}};

const std::array<Section, 3> stream{{
    {"Health",     Viz::Kv, &stream_health},
    {"Timing",     Viz::Kv, &stream_timing},
    {"Throughput", Viz::Kv, &stream_throughput},
}};

const std::array<Section, 2> context{{
    {"",      Viz::Kv,   &context_rows},
    {"Trend", Viz::Plot, &context_plot},
}};

const std::array<Section, 1> retrieval{{
    {"", Viz::Kv, &retrieval_rows},
}};

}  // namespace sections
}  // namespace agentty::stats
