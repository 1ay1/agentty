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

// Where the wall-clock actually went. A session's total time is the least
// actionable number on the panel — "1m05s" tells you nothing you can fix.
// The SPLIT does: waiting on the provider, generating, and running tools
// have three different causes and three different remedies.
void session_where_time_went(const Facts& f, std::vector<Metric>& out) {
    const double ttft  = static_cast<double>(f.stream.ttft.sum());
    const double gen   = static_cast<double>(f.stream.stream_ms.sum());
    const double tools = static_cast<double>(f.tools.latency.sum());
    const double all   = ttft + gen + tools;
    if (all <= 0) return;
    Metric m;
    m.label  = "where the time went";
    m.unit   = Unit::Millis;
    m.value  = all;
    m.detail = format(Unit::Millis, all);
    // Same for the time ring: shares as percentages, durations below.
    auto share = [&](double v) { return format(Unit::Ratio, v / all); };
    m.parts = {
        {"wait "  + share(ttft),  ttft,  kWarn},
        {"gen "   + share(gen),   gen,   kAccent},
        {"tools " + share(tools), tools, kGood},
    };
    out.push_back(std::move(m));
}

void session_time(const Facts& f, std::vector<Metric>& out) {
    if (!f.session.measured_turns) return;
    // The absolute durations the ring above shows as shares. Kept as a
    // separate section so the ring stays readable: a legend carrying both
    // "48%" and "59s" is a legend nobody finishes reading.
    if (f.stream.ttft.sum())
        kv(out, "Waiting",    Unit::Millis, static_cast<double>(f.stream.ttft.sum()));
    if (f.stream.stream_ms.sum())
        kv(out, "Generating", Unit::Millis, static_cast<double>(f.stream.stream_ms.sum()));
    if (f.tools.latency.sum())
        kv(out, "Tools",      Unit::Millis, static_cast<double>(f.tools.latency.sum()));
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
    // The hit rate goes in the ring's centre: it is the tab's answer, and
    // a reader looking at a ring is already looking at the hole.
    m.detail = format(Unit::Ratio,
                      static_cast<double>(f.cache.hits) / total) + " hit";
    // Percentages, not raw counts, in the legend. A ring already shows the
    // proportions as ANGLES — restating them as token counts makes the
    // reader do the division the chart just did for them. The absolute
    // numbers live one section down, where they can be compared exactly.
    auto pct = [&](std::uint64_t n) {
        return format(Unit::Ratio, static_cast<double>(n) / total);
    };
    m.parts = {
        {"hit "   + pct(f.cache.hits),   static_cast<double>(f.cache.hits),   kGood},
        {"write " + pct(f.cache.writes), static_cast<double>(f.cache.writes), kWarn},
        {"miss "  + pct(f.cache.misses), static_cast<double>(f.cache.misses), kBad},
    };
    out.push_back(std::move(m));
}

void cache_rates(const Facts& f, std::vector<Metric>& out) {
    const double prefix = static_cast<double>(f.cache.hits + f.cache.writes
                                            + f.cache.misses);
    if (prefix <= 0) return;
    // The hit rate carries its own per-turn trend. A session average hides
    // the shape that decides what to do about it: a cache that WARMED (low
    // then high, the healthy case) and one that keeps missing on every
    // turn can report the same number.
    out.push_back(Metric{
        .label  = "Hit rate",
        .unit   = Unit::Ratio,
        .value  = static_cast<double>(f.cache.hits) / prefix,
        .detail = "per turn \xe2\x86\x92",
        .series = f.cache.ratio_series});
    kv(out, "Turns using cache", Unit::Count,
       static_cast<double>(f.cache.turns_with_cache));
}

// The absolute counts the ring shows as angles. Its own section because
// the ring's legend carries percentages — a key holding both "80%" and
// "93k" is one nobody finishes reading — and because these three share a
// unit, so they get a common bar scale here and can be compared exactly.
void cache_tokens(const Facts& f, std::vector<Metric>& out) {
    if (f.cache.hits + f.cache.writes + f.cache.misses == 0) return;
    // Cache reads are ~10% the price of fresh input on every provider that
    // offers them, so this first row is the single biggest lever on a long
    // thread's cost.
    kv(out, "Served from cache", Unit::Tokens, static_cast<double>(f.cache.hits));
    kv(out, "Written to cache",  Unit::Tokens, static_cast<double>(f.cache.writes));
    kv(out, "Sent uncached",     Unit::Tokens, static_cast<double>(f.cache.misses));
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
    kv(out, "Median",  Unit::Millis, h.quantile(0.5));
    kv(out, "p95",     Unit::Millis, h.quantile(0.95));
    kv(out, "Slowest", Unit::Millis, static_cast<double>(h.max()));
    kv(out, "Total",   Unit::Millis, static_cast<double>(h.sum()));
}

// One row per occupied bucket of a histogram. The bucket's RANGE is the
// label, its count the value — so the reader sees where the samples
// actually landed instead of two quantiles standing in for a shape.
//
// Takes the unit because a histogram is unit-agnostic: the same log2
// buckets hold milliseconds, bytes or tokens, and only the caller knows
// which. Hard-coding Millis here is how a byte spread ends up labelled
// "16ms–32ms".
void dist_rows(const Hist& h, std::vector<Metric>& out,
               Unit u = Unit::Millis) {
    if (!h.count()) return;
    const auto [lo, hi] = h.occupied();
    if (hi < lo) return;
    const double total = static_cast<double>(h.count());
    double peak = 0;
    for (int b = lo; b <= hi; ++b)
        if (h.buckets()[static_cast<std::size_t>(b)] > peak)
            peak = h.buckets()[static_cast<std::size_t>(b)];
    for (int b = lo; b <= hi; ++b) {
        const double n = h.buckets()[static_cast<std::size_t>(b)];
        // Empty buckets INSIDE the occupied range are kept: a gap is a
        // real feature of a bimodal distribution (fast local calls, slow
        // network ones) and closing it up hides exactly that.
        const auto floor_v = Hist::bucket_floor(b);
        const auto ceil_v  = Hist::bucket_floor(b + 1);
        out.push_back(Metric{
            .label  = format(u, floor_v) + "\xe2\x80\x93" + format(u, ceil_v),
            .unit   = Unit::Count,
            .value  = n,
            .of     = peak,
            .detail = n > 0 ? format(Unit::Ratio, n / total) : ""});
    }
}

void tools_latency_dist(const Facts& f, std::vector<Metric>& out) {
    dist_rows(f.tools.latency, out);
}

void stream_ttft_dist(const Facts& f, std::vector<Metric>& out) {
    dist_rows(f.stream.ttft, out);
}

// How big each turn's frame payload was. Bytes and TOKENS answer
// different questions and only one of them is billed — a spread that is
// tight says the provider frames consistently, a bimodal one says short
// acknowledgements and long generations are arriving through the same
// path, which is what a stalled-looking stream usually turns out to be.
void stream_bytes_dist(const Facts& f, std::vector<Metric>& out) {
    dist_rows(f.stream.bytes, out, Unit::Bytes);
}

// ── Reasoning ────────────────────────────────────────────────────────────

// The verdict. "Is extended thinking earning its keep" is a question
// about SHARE: what fraction of the output went to reasoning tokens the
// user never reads. Under ~10% effort is barely engaging; over ~50% the
// model is spending most of its budget deliberating.
void reasoning_hero(const Facts& f, std::vector<Metric>& out) {
    if (!f.reasoning.turns) return;
    const double share = f.tokens.output
        ? static_cast<double>(f.reasoning.tokens)
            / static_cast<double>(f.tokens.output)
        : 0.0;
    out.push_back(Metric{
        .label  = format(Unit::Ratio, share),
        .unit   = Unit::Ratio,
        .value  = share,
        .detail = "of generated tokens were reasoning"});
}

void reasoning_rows(const Facts& f, std::vector<Metric>& out) {
    kv(out, "Turns that thought", Unit::Count,
       static_cast<double>(f.reasoning.turns),
       format(Unit::Ratio,
              f.session.assistant_turns
                ? static_cast<double>(f.reasoning.turns)
                    / static_cast<double>(f.session.assistant_turns)
                : 0.0));
    // The split that says what the thinking was FOR. Deliberating and
    // then calling a tool is reasoning about what to do; deliberating and
    // only answering is reasoning instead of doing. Neither is wrong, but
    // the ratio is the difference between effort buying decisions and
    // effort buying prose.
    if (f.reasoning.turns) {
        kv(out, "…then acted", Unit::Count,
           static_cast<double>(f.reasoning.thought_then_acted),
           format(Unit::Ratio,
                  static_cast<double>(f.reasoning.thought_then_acted)
                    / static_cast<double>(f.reasoning.turns)));
        kv(out, "…then answered", Unit::Count,
           static_cast<double>(f.reasoning.turns
                             - f.reasoning.thought_then_acted));
    }
    if (f.reasoning.blocks)
        kv(out, "Thinking blocks", Unit::Count,
           static_cast<double>(f.reasoning.blocks),
           f.reasoning.turns
             ? format(Unit::Count,
                      static_cast<double>(f.reasoning.blocks)
                        / static_cast<double>(f.reasoning.turns)) + " per turn"
             : "");
}

void reasoning_time(const Facts& f, std::vector<Metric>& out) {
    if (!f.reasoning.ms) return;
    kv(out, "Total",   Unit::Millis, static_cast<double>(f.reasoning.ms));
    kv(out, "Average", Unit::Millis, f.reasoning.per_turn_ms.mean());
    kv(out, "Median",  Unit::Millis, f.reasoning.per_turn_ms.quantile(0.5));
    kv(out, "Longest", Unit::Millis,
       static_cast<double>(f.reasoning.longest_ms));
}

void reasoning_tokens(const Facts& f, std::vector<Metric>& out) {
    if (!f.reasoning.tokens) return;
    kv(out, "Reasoning tokens", Unit::Tokens,
       static_cast<double>(f.reasoning.tokens));
    if (f.reasoning.per_turn_tokens.count()) {
        kv(out, "Average per turn", Unit::Tokens,
           f.reasoning.per_turn_tokens.mean());
        kv(out, "Deepest turn", Unit::Tokens,
           static_cast<double>(f.reasoning.per_turn_tokens.max()));
    }
}

// Effort is supposed to ADAPT: hard turns think longer, easy ones barely
// at all. A flat line here means it is not adapting, which is the one
// finding that would send a user to the Smart Mode settings.
void reasoning_trend(const Facts& f, std::vector<Metric>& out) {
    if (f.reasoning.ms_series.size() < 2) return;
    out.push_back(Metric{.label  = "thinking time per turn",
                         .unit   = Unit::Millis,
                         .series = f.reasoning.ms_series});
}

// Where the thinking time actually landed. An average of 2.4s could be
// two turns at 2.4s or one at 200ms and one at 4.6s, and only the second
// shape says the effort setting is doing anything.
void reasoning_dist(const Facts& f, std::vector<Metric>& out) {
    dist_rows(f.reasoning.per_turn_ms, out);
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

// The RATES, each with its own per-turn trend. A rate is the one kind of
// number where the average is actively misleading: "1.0k/s" over a
// session that started fast and degraded is a figure that never happened,
// and the sparkline is what shows it.
void stream_rates(const Facts& f, std::vector<Metric>& out) {
    if (!f.stream.ttft_series.empty()) {
        out.push_back(Metric{
            .label  = "First byte",
            .unit   = Unit::Millis,
            .value  = f.stream.ttft.mean(),
            .detail = "avg",
            .series = f.stream.ttft_series});
    }
    if (!f.stream.rate_series.empty()) {
        double sum = 0;
        for (double v : f.stream.rate_series) sum += v;
        out.push_back(Metric{
            .label  = "Output rate",
            .unit   = Unit::Rate,
            .value  = sum / static_cast<double>(f.stream.rate_series.size()),
            .detail = "tok/s",
            .series = f.stream.rate_series});
    }
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
    if (f.retrieval.with_confidence) {
        const double avg = f.retrieval.confidence_sum
                         / static_cast<double>(f.retrieval.with_confidence);
        // A confidence is a share of a KNOWN whole (1.0), so unlike the
        // section-scaled bars this one is measured against the real
        // ceiling — "82% confident" against a full meter says whether the
        // funnel was sure, where 82% of the section's largest ratio would
        // always read as full.
        out.push_back(Metric{.label = "Average confidence",
                             .unit  = Unit::Ratio,
                             .value = avg,
                             .of    = 1.0});
    }
}

}  // namespace

namespace sections {

const std::array<Section, 4> session{{
    {"",          Viz::Hero,  &session_hero},
    {"Activity",  Viz::Kv,    &session_counts},
    {"",          Viz::Donut, &session_where_time_went},
    {"Time",      Viz::Kv,    &session_time},
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

const std::array<Section, 3> cache{{
    {"",       Viz::Donut, &cache_band},
    {"Tokens", Viz::Kv,    &cache_tokens},
    {"Rates",  Viz::Spark, &cache_rates},
}};

const std::array<Section, 4> tools{{
    {"",             Viz::Band, &tools_status},
    {"By tool",      Viz::Bars, &tools_by_name},
    {"Latency",      Viz::Kv,   &tools_latency},
    {"Distribution", Viz::Hist, &tools_latency_dist},
}};

const std::array<Section, 6> reasoning{{
    {"",             Viz::Hero,  &reasoning_hero},
    {"Turns",        Viz::Kv,    &reasoning_rows},
    {"Time",         Viz::Kv,    &reasoning_time},
    {"Tokens",       Viz::Kv,    &reasoning_tokens},
    {"Trend",        Viz::Plot,  &reasoning_trend},
    {"Spread",       Viz::Hist,  &reasoning_dist},
}};

const std::array<Section, 6> stream{{
    {"Health",            Viz::Kv,    &stream_health},
    {"Rates",             Viz::Spark, &stream_rates},
    {"Timing",            Viz::Kv,    &stream_timing},
    {"Throughput",        Viz::Kv,    &stream_throughput},
    {"First byte spread", Viz::Hist,  &stream_ttft_dist},
    {"Frame size spread", Viz::Hist,  &stream_bytes_dist},
}};

const std::array<Section, 2> context{{
    {"",      Viz::Kv,   &context_rows},
    {"Trend", Viz::Plot, &context_plot},
}};

const std::array<Section, 1> retrieval{{
    {"", Viz::Bars, &retrieval_rows},
}};

}  // namespace sections
}  // namespace agentty::stats
