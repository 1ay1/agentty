// util/storage_env.cpp — settings.json "dirs" -> AGENTTY_<NAME>_DIR.

#include "agentty/util/storage_env.hpp"

#include "agentty/util/home_dir.hpp"

#include <nlohmann/json.hpp>

#include <array>
#include <cstdlib>
#include <filesystem>
#include <fstream>

namespace agentty::util {

namespace fs = std::filesystem;

namespace {

constexpr std::array kKeys{
    DirKey{"threads",       "AGENTTY_THREADS_DIR"},
    DirKey{"credentials",   "AGENTTY_CREDENTIALS_DIR"},
    DirKey{"state",         "AGENTTY_STATE_DIR"},
    DirKey{"cache",         "AGENTTY_CACHE_DIR"},
    DirKey{"logs",          "AGENTTY_LOGS_DIR"},
    DirKey{"project",       "AGENTTY_PROJECT_DIR"},
    DirKey{"rag",           "AGENTTY_RAG_DIR"},
    DirKey{"project_state", "AGENTTY_PROJECT_STATE_DIR"},
};

// Same rule as user_root(), but read-only: nothing is created here.
fs::path settings_file() {
    if (const char* h = std::getenv("AGENTTY_HOME"); h && *h)
        return fs::path{h} / "settings.json";
    const fs::path home = home_dir_or_empty();
    return home.empty() ? fs::path{} : home / ".agentty" / "settings.json";
}

}  // namespace

std::span<const DirKey> dir_keys() noexcept { return {kKeys.data(), kKeys.size()}; }

std::vector<DirAssignment> settings_dir_assignments() {
    std::vector<DirAssignment> out;
    const fs::path p = settings_file();
    std::error_code ec;
    if (p.empty() || !fs::is_regular_file(p, ec)) return out;
    std::ifstream in(p);
    const auto j = nlohmann::json::parse(in, nullptr, /*allow_exceptions=*/false);
    if (!j.is_object()) return out;
    const auto it = j.find("dirs");
    if (it == j.end() || !it->is_object()) return out;
    for (const auto& k : kKeys) {
        const auto v = it->find(std::string{k.key});
        if (v == it->end() || !v->is_string()) continue;
        std::string val = v->get<std::string>();
        if (val.empty()) continue;
        // ~/x is common in a hand-written config; the shell isn't there to expand it.
        if (val == "~" || val.starts_with("~/")) {
            const fs::path home = home_dir_or_empty();
            if (home.empty()) continue;
            val = (home / val.substr(val.size() > 1 ? 2 : 1)).string();
        }
        const std::string env{k.env};
        if (const char* cur = std::getenv(env.c_str()); cur && *cur) continue;
        out.push_back({env, std::move(val)});
    }
    return out;
}

}  // namespace agentty::util
