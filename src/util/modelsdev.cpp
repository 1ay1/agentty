// agentty::modelsdev — background snapshot of models.dev (see header).

#include "agentty/util/modelsdev.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string_view>

#include <nlohmann/json.hpp>

#include "agentty/auth/auth.hpp"        // config_dir()
#include "agentty/domain/catalog.hpp"   // set_catalog_reasoning / set_catalog_effort_set
#include "agentty/io/http.hpp"
#include "agentty/util/dbglog.hpp"

namespace agentty::modelsdev {

namespace {

using json = nlohmann::json;
namespace fs = std::filesystem;

fs::path cache_path() { return auth::config_dir() / "modelsdev.json"; }

constexpr auto kMaxAge = std::chrono::hours{24};
constexpr std::size_t kMaxBody = 8ull * 1024 * 1024;   // api.json is ~2 MB

// Map a models.dev effort value name to our Effort bit; 0 = not in our ladder
// (e.g. "minimal", "ultra") — ignored, the nearest_effort clamp covers gaps.
std::uint8_t bit_of(std::string_view v) {
    using agentty::Effort;
    using agentty::effort_bit;
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
void register_model(const std::string& dev_provider,
                    const std::string& mid, const json& m) {
    if (!m.is_object()) return;
    const auto reasoning = m.find("reasoning");
    if (reasoning == m.end() || !reasoning->is_boolean()) return;

    // Effort enum: reasoning_options entries of type "effort" carry the
    // exact value list. A reasoning model WITHOUT an effort option reasons
    // natively with no control — declared reasoning true but effort set 0
    // would hide the (harmless, display-only) toggle, so only record a set
    // when values are actually named.
    int set = -1;
    if (auto ro = m.find("reasoning_options");
        ro != m.end() && ro->is_array()) {
        for (const auto& opt : *ro) {
            if (!opt.is_object()) continue;
            if (opt.value("type", std::string{}) != "effort") continue;
            std::uint8_t bits = 0;
            for (const auto& v : opt.value("values", json::array()))
                if (v.is_string()) bits |= bit_of(v.get<std::string>());
            if (bits != 0) set = bits;
        }
    }

    // The model id as agentty sees it on that provider's wire: the tail
    // after any prefix (openrouter/nano-gpt style keys embed the upstream
    // vendor as a path segment, which is part of THEIR wire id — keep the
    // full mid for the scoped key, and use the tail only for bare merging).
    const std::string bare = [&] {
        auto slash = mid.rfind('/');
        return slash == std::string::npos ? mid : mid.substr(slash + 1);
    }();

    // Authoritative, collision-free: "<models.dev provider>/<mid>".
    if (!dev_provider.empty()) {
        set_catalog_reasoning(dev_provider + "/" + mid,
                              reasoning->get<bool>());
        if (set >= 0)
            set_catalog_effort_set(dev_provider + "/" + mid,
                                   static_cast<std::uint8_t>(set));
        // Aggregator-style mids ("mistralai/mistral-medium-3-5"): also
        // record under "<provider>/<tail>" so agentty's scoped lookup —
        // which uses the wire model id — hits when the user's endpoint
        // strips the vendor prefix.
        if (bare != mid) {
            set_catalog_reasoning(dev_provider + "/" + bare,
                                  reasoning->get<bool>());
            if (set >= 0)
                set_catalog_effort_set(dev_provider + "/" + bare,
                                       static_cast<std::uint8_t>(set));
        }
    }

    // Bare key: SHARED across every provider — merge-or-poison only.
    if (!bare.empty()) {
        merge_catalog_reasoning(bare, reasoning->get<bool>());
        if (set >= 0)
            merge_catalog_effort_set(bare, static_cast<std::uint8_t>(set));
    }
}

// Parse a full api.json document; returns models registered.
int load_document(const std::string& body) {
    int n = 0;
    try {
        json j = json::parse(body);
        if (!j.is_object()) return 0;
        for (const auto& [pid, provider] : j.items()) {
            if (!provider.is_object()) continue;
            auto models = provider.find("models");
            if (models == provider.end() || !models->is_object()) continue;
            for (const auto& [mid, m] : models->items()) {
                register_model(pid, mid, m);
                ++n;
            }
        }
    } catch (const std::exception& e) {
        util::dbglog("modelsdev.parse", e.what());
        return 0;
    } catch (...) {
        return 0;
    }
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

void load_cached() {
    std::ifstream ifs(cache_path(), std::ios::binary);
    if (!ifs) return;
    std::ostringstream ss;
    ss << ifs.rdbuf();
    load_document(ss.str());
}

int refresh() {
    if (cache_fresh()) {
        // Fresh on disk — (re)load in case this process hasn't yet.
        std::ifstream ifs(cache_path(), std::ios::binary);
        if (ifs) {
            std::ostringstream ss;
            ss << ifs.rdbuf();
            return load_document(ss.str());
        }
    }
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
    auto resp = http::default_client().send(req, tos);
    if (!resp || resp->status != 200 || resp->body.empty()) {
        // Network down / endpoint moved: fall back to whatever is cached.
        load_cached();
        return 0;
    }
    const int n = load_document(resp->body);
    if (n > 0) {
        // Atomic-ish write: tmp + rename, matching the rest of the config dir.
        std::error_code ec;
        fs::create_directories(auth::config_dir(), ec);
        const auto tmp = cache_path().string() + ".tmp";
        {
            std::ofstream ofs(tmp, std::ios::binary | std::ios::trunc);
            if (!ofs) return n;
            ofs << resp->body;
        }
        fs::rename(tmp, cache_path(), ec);
        if (ec) fs::remove(tmp, ec);
    }
    return n;
}

} // namespace agentty::modelsdev
