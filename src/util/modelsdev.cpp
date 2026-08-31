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

// Register one model's declaration under BOTH its scoped and bare id.
void register_model(const std::string& scoped_id, const json& m) {
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

    auto record = [&](const std::string& id) {
        if (id.empty()) return;
        set_catalog_reasoning(id, reasoning->get<bool>());
        if (set >= 0)
            set_catalog_effort_set(id, static_cast<std::uint8_t>(set));
    };
    record(scoped_id);
    // Bare tail: "deepseek/deepseek-v4-flash" → "deepseek-v4-flash". This key
    // is SHARED across every models.dev provider, so different aggregators
    // can disagree about the same bare id (one lists mistral-large-2402 as
    // reasoning, another as instruct-only). A plain last-writer-wins record
    // here is exactly what lit a phantom reasoning chip on Mistral Large 3.
    // Merge instead: agree → keep, disagree → poison the key to "no info" so
    // resolution falls back to the scoped fact or id-inference. No hardcoded
    // ids — the collision resolves itself by the sources disagreeing.
    if (auto slash = scoped_id.rfind('/'); slash != std::string::npos) {
        const std::string bare = scoped_id.substr(slash + 1);
        if (!bare.empty()) {
            merge_catalog_reasoning(bare, reasoning->get<bool>());
            if (set >= 0)
                merge_catalog_effort_set(bare, static_cast<std::uint8_t>(set));
        }
    }
}

// Parse a full api.json document; returns models registered.
int load_document(const std::string& body) {
    int n = 0;
    try {
        json j = json::parse(body);
        if (!j.is_object()) return 0;
        for (const auto& [_, provider] : j.items()) {
            if (!provider.is_object()) continue;
            auto models = provider.find("models");
            if (models == provider.end() || !models->is_object()) continue;
            for (const auto& [mid, m] : models->items()) {
                register_model(mid, m);
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
