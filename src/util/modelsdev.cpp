// agentty::modelsdev — background snapshot of models.dev (see header).

#include "agentty/util/modelsdev.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <stop_token>
#include <string_view>

#include <maya/runtime.hpp>

#include <simdjson.h>

#include "agentty/util/user_root.hpp"    // user_cache_dir()
#include <limits>
#include "agentty/domain/catalog.hpp"   // set_catalog_reasoning / set_catalog_effort_set
#include "agentty/io/http.hpp"
#include "agentty/io/shared_file.hpp"
#include "agentty/util/dbglog.hpp"
#include "agentty/util/teardown.hpp"

namespace agentty::modelsdev {

namespace {

namespace fs = std::filesystem;

fs::path cache_path() { return util::user_cache_dir() / "modelsdev.json"; }

constexpr auto kMaxAge = std::chrono::hours{24};
constexpr std::size_t kMaxBody = 8ull * 1024 * 1024;   // api.json is ~2 MB

// Map a models.dev effort value name to our Effort bit; 0 = not in our
// ladder (e.g. "ultra") — ignored, the nearest_effort clamp covers gaps.
std::uint8_t bit_of(std::string_view v) {
    using agentty::Effort;
    using agentty::effort_bit;
    if (v == "minimal") return effort_bit(Effort::Minimal);
    if (v == "low")    return effort_bit(Effort::Low);
    if (v == "medium") return effort_bit(Effort::Medium);
    if (v == "high")   return effort_bit(Effort::High);
    if (v == "xhigh")  return effort_bit(Effort::Xhigh);
    if (v == "max")    return effort_bit(Effort::Max);
    return 0;
}

// Register one model's declaration.
//
// KEYING IS THE WHOLE GAME here. models.dev model keys are only SOMETIMES
// provider-prefixed ("mistral-medium-3-5" on requesty, "mistral/mistral-
// medium-3.5" on nano-gpt, "mistralai/mistral-medium-3-5" on openrouter),
// and agentty's capability registries use "provider/model" scoped keys. Two
// bugs fell out of treating the raw key as already-scoped:
//   • an unprefixed key from an AGGREGATOR (requesty's full low..max effort
//     ladder for a model that is BINARY on Mistral's own platform) was
//     recorded via the direct setter and matched agentty's bare-id fallback
//     — the wrong ladder for the real provider;
//   • a "mistral/..."-prefixed key from nano-gpt collided byte-for-byte
//     with agentty's OWN "mistral/<model>" scope namespace — a third
//     party's declaration masquerading as the Mistral-platform fact.
// Now: the authoritative record is ALWAYS scoped under the models.dev
// PROVIDER id ("mistral/mistral-medium-2604", "requesty/mistral-medium-
// 3-5") — which for real platforms (mistral, openai, groq, cerebras…)
// exactly matches agentty's endpoint-label scope, so the active provider's
// scoped lookup hits the fact from ITS OWN models.dev entry and never an
// aggregator's. Bare tails go ONLY through the merge-or-poison path:
// cross-provider agreement keeps the fact, any disagreement poisons the
// bare key to "no info" so resolution falls to the scoped fact or id
// inference. (Your requesty-vs-openrouter disagreement on mistral-medium-
// 3-5 now poisons the bare key, and id inference's effort_high_only
// correctly yields the {off, high} ladder.)
void register_model(CatalogBatch& out,
                    const std::string& dev_provider,
                    const std::string& dev_env0,
                    const std::string& dev_api,
                    const std::string& mid, simdjson::dom::element m) {
    bool reasons = false;
    if (m["reasoning"].get_bool().get(reasons)) return;   // absent or not a bool

    // Effort enum: reasoning_options entries of type "effort" carry the
    // exact value list. A reasoning model WITHOUT an effort option reasons
    // natively with no control — declared reasoning true but effort set 0
    // would hide the (harmless, display-only) toggle, so only record a set
    // when values are actually named. An oddly typed entry is skipped: one
    // bad field must not cost the rest of the document.
    int set = -1;
    if (simdjson::dom::array ro; !m["reasoning_options"].get_array().get(ro)) {
        for (const simdjson::dom::element opt : ro) {
            std::string_view type;
            if (opt["type"].get_string().get(type)) continue;
            if (type == "effort") {
                std::uint8_t bits = 0;
                if (simdjson::dom::array vals; !opt["values"].get_array().get(vals))
                    for (const simdjson::dom::element v : vals)
                        if (std::string_view s; !v.get_string().get(s)) bits |= bit_of(s);
                if (bits != 0) set = bits;
            } else if (type == "toggle" && set < 0) {
                // Toggle-only reasoning (Kimi k2.5, some Qwen): on/off with
                // no tiers. Model it as the binary {high} mask — the ladder
                // renders off·high (an honest on/off switch) and any
                // requested tier clamps to "on". An effort option in the
                // same list wins (k3 declares BOTH toggle and effort).
                set = static_cast<int>(
                    agentty::effort_bit(agentty::Effort::High));
            }
        }
    }

    // The scope for these records: agentty's OWN provider id when the
    // models.dev entry's identity resolves to a registry row (exact id →
    // primary env var → api host, see capkey::resolve_dev_provider), else
    // the inert "dev:<id>" namespace agentty's lookups can never collide
    // with — unknown aggregators' facts are preserved but harmless.
    const std::string scope = capkey::dev_scope(dev_provider, dev_env0,
                                                dev_api);

    // Scoped records: under the full mid AND — for vendor-prefixed keys
    // ("mistralai/mistral-medium-3-5") — under the tail, since agentty's
    // scoped lookup uses the wire model id which may omit the vendor path.
    // capkey normalisation inside the setters folds spelling variants.
    // Context window, from `limit.context`. Same scoping as the facts
    // above: the scoped record is authoritative, the shared bare key is
    // merge-or-poison. Guarded on a positive integer — a null/absent/0
    // limit is "not declared", and recording 0 would be indistinguishable
    // from a declaration of nothing.
    int dev_ctx = 0;
    // Signed or unsigned integer, positive and in int range. A float, string
    // or out-of-range value is "not declared".
    if (std::int64_t v = 0; !m["limit"]["context"].get_int64().get(v)) {
        if (v > 0 && v <= std::numeric_limits<int>::max())
            dev_ctx = static_cast<int>(v);
    }

    out.set_reasoning(scope + "/" + mid, reasons);
    if (dev_ctx > 0) out.set_context_window(scope + "/" + mid, dev_ctx);
    if (set >= 0)
        out.set_effort_set(scope + "/" + mid, static_cast<std::uint8_t>(set));
    // CUSTOM-HOST seam: index the declaration under the entry's api ORIGIN
    // as well, so a live localhost / custom-host catalog whose URL matches
    // can claim the figure (the provider-scoped key above can never reach a
    // custom host, whose provider id IS its endpoint spec). The provider-
    // scoped write stays the primary channel for presets.
    if (dev_ctx > 0 && !dev_api.empty())
        out.merge_endpoint_context_window(dev_api, mid, dev_ctx);
    const std::string tail = capkey::norm_tail(mid);
    if (tail != capkey::norm_model(mid)) {
        out.set_reasoning(scope + "/" + tail, reasons);
        if (dev_ctx > 0) out.set_context_window(scope + "/" + tail, dev_ctx);
        if (set >= 0)
            out.set_effort_set(scope + "/" + tail, static_cast<std::uint8_t>(set));
    }

    // Bare key: SHARED across every provider — merge-or-poison only
    // (agree → keep, disagree → no-info; RULE 3 in capkey.hpp).
    if (!tail.empty()) {
        out.merge_reasoning(tail, reasons);
        if (dev_ctx > 0) out.merge_context_window(tail, dev_ctx);
        if (set >= 0)
            out.merge_effort_set(tail, static_cast<std::uint8_t>(set));
    }
}

// Parse a full api.json document into `batch`; returns models registered.
// The caller publishes the batch once, so a reader never sees half a
// document and the load is linear, not quadratic.
//
// simdjson, not nlohmann: building the 5+ MB document as a std::map tree was
// most of the remaining startup CPU. Every field is read by type and an
// oddly typed one is skipped. nlohmann threw on those (a numeric option
// "type" or provider "api") and the catch discarded the WHOLE document, so
// one malformed upstream entry silently cost every model.
int parse_document(std::string_view body, CatalogBatch& batch) {
    simdjson::dom::parser parser;
    const simdjson::padded_string padded{body};
    simdjson::dom::object root;
    if (const auto err = parser.parse(padded).get_object().get(root)) {
        util::dbglog("modelsdev.parse", simdjson::error_message(err));
        return 0;
    }
    int n = 0;
    for (const auto provider : root) {
        simdjson::dom::object models;
        if (provider.value["models"].get_object().get(models)) continue;
        // Identity anchors for capkey::dev_scope (see capkey.hpp): the
        // entry's FIRST env var + api URL, empty when absent or not text.
        std::string env0;
        if (std::string_view s; !provider.value["env"].at(0).get_string().get(s)) env0 = s;
        std::string api;
        if (std::string_view s; !provider.value["api"].get_string().get(s)) api = s;
        const std::string pid{provider.key};
        for (const auto model : models) {
            register_model(batch, pid, env0, api, std::string{model.key}, model.value);
            ++n;
        }
    }
    return n;
}

int load_document(std::string_view body) {
    CatalogBatch batch;
    const int n = parse_document(body, batch);
    if (n > 0) batch.commit();
    return n;
}

bool cache_fresh() {
    std::error_code ec;
    const auto mtime = fs::last_write_time(cache_path(), ec);
    if (ec) return false;
    const auto age = fs::file_time_type::clock::now() - mtime;
    return age < kMaxAge;
}

} // namespace

int parse_into(std::string_view body, CatalogBatch& out) {
    return parse_document(body, out);
}

void load_cached() {
    std::ifstream ifs(cache_path(), std::ios::binary);
    if (!ifs) return;
    std::ostringstream ss;
    ss << ifs.rdbuf();
    load_document(ss.str());
}

int refresh() {
    // A fresh cache is already loaded (start_background_refresh loads it
    // first); parsing the same 5 MB twice was pure waste.
    if (cache_fresh()) return 0;
    http::Request req;
    req.method = http::HttpMethod::Get;
    req.host   = "models.dev";
    req.port   = 443;
    req.path   = "/api.json";
    req.headers = {
        {"accept", "application/json"},
        {"user-agent", "agentty/" AGENTTY_VERSION},
    };
    req.max_body_bytes = kMaxBody;
    http::Timeouts tos;
    tos.connect = std::chrono::milliseconds(8'000);
    tos.total   = std::chrono::milliseconds(60'000);
    auto resp = http::default_client(::agentty::IoAccess::grant()).send(req, tos);
    if (!resp || resp->status != 200 || resp->body.empty())
        return 0;   // network down / moved: the cached load already stands
    const int n = load_document(resp->body);
    if (n > 0) {
        // A cache: atomic so a reader never sees half of it, no fsync.
        (void)persistence::write_atomic(::agentty::IoAccess::grant(), cache_path(), resp->body,
                                        {.durability = persistence::Durability::Atomic});
    }
    return n;
}

// ── Background refresh (same pattern as blob_gc.cpp) ────────────────
namespace {
// This subsystem's OWN pool: join_background_refresh() promises ITS fetch is
// done, and a shared pool cannot be joined one job at a time. See blob_gc.cpp
// for the same reasoning — these two stay deliberately identical.
//
// Replaces a mutex + condition variable + raw worker + stop flag whose
// shutdown detached the worker whenever the fetch was still in flight, which
// is precisely a network call left running against statics the CRT is
// destroying. pool::shutdown asks it to stop, waits inside a bounded grace,
// then abandons safely.
maya::pool& refresh_pool() {
    static maya::pool p{/*max_workers=*/1};   // one refresh, ever
    return p;
}
constexpr auto kStartDelay = std::chrono::seconds(5);
}  // namespace

void start_background_refresh(bool no_net) {
    // Register the join the moment background work is about to exist, rather
    // than relying on main() to know this subsystem is threaded. main()'s
    // hand-written teardown list is the thing that misses one: settings_cache
    // shipped with a join nobody called, and its worker sat joinable in a
    // function-local static until the destructor terminated the process at
    // exit.
    //
    // refresh_pool() is a function-local static that lives to process exit, so
    // the callback can never outlive its target and needs no cancel(). The
    // whole start runs once per process.
    static std::once_flag once;
    std::call_once(once, [no_net] {
        util::teardown::on_shutdown("modelsdev.refresh",
                                    [](const maya::unlocked& cap) { join_background_refresh(cap); });

    // Isolated: refresh() is a network fetch, so it may never return and must
    // not occupy a shared worker.
    refresh_pool().post_isolated([](std::stop_token st, bool no_net) {
        // A process that exits at once never dials at all. One cancellation
        // channel — the token — instead of a second stop flag to notify.
        if (maya::delay_for(st, kStartDelay)) return;
        try {
            load_cached();
            // Re-check: the cache load is the cheap half, and shutdown may
            // have been requested while it ran. No point starting a dial we
            // are about to abandon.
            if (!no_net && !st.stop_requested()) {
                try { (void)refresh(); } catch (...) {}
            }
        } catch (const std::exception& e) {
            util::dbglog("modelsdev.refresh", e.what());
        } catch (...) {}
    }, no_net);
    });
}

void join_background_refresh(const maya::unlocked& cap) noexcept {
    (void)refresh_pool().shutdown(cap);   // requests stop, waits within the grace
}

} // namespace agentty::modelsdev
