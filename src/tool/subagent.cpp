#include "agentty/tool/subagent.hpp"

#include <atomic>
#include <mutex>

namespace agentty::tools::subagent {

namespace {
Config     g_cfg;
std::mutex g_mu;
// Per-thread nesting depth. Each subagent runs synchronously on its own
// (task_isolated) worker thread, so thread_local correctly scopes the
// depth to one chain of nested subagents.
thread_local int g_depth = 0;
} // namespace

void install(Config cfg) {
    cfg.installed = true;
    std::lock_guard lk(g_mu);
    g_cfg = std::move(cfg);
}

Config current() {
    std::lock_guard lk(g_mu);
    return g_cfg;
}

void set_auth(auth::AuthHeader auth) {
    std::lock_guard lk(g_mu);
    if (!g_cfg.installed) return;
    g_cfg.auth = std::move(auth);
}

void set_model(std::string model) {
    if (model.empty()) return;
    std::lock_guard lk(g_mu);
    // Only meaningful once a config exists; leave `installed` untouched.
    g_cfg.model = std::move(model);
}

void set_candidates(std::vector<ModelInfo> candidates) {
    std::lock_guard lk(g_mu);
    if (!g_cfg.installed) return;
    g_cfg.candidates = std::move(candidates);
}

int current_depth() noexcept { return g_depth; }
void push_depth() noexcept { ++g_depth; }
void pop_depth() noexcept { if (g_depth > 0) --g_depth; }

} // namespace agentty::tools::subagent
