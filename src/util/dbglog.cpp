#include "agentty/util/dbglog.hpp"

#include "agentty/util/logx.hpp"

namespace agentty::util {

// dbglog is a COMPAT SHIM over logx (the structured log).
//
// Every call site tags itself with a dotted site name ("persistence.save",
// "anthropic.list_models.parse", "mcp.bridge.connect"). That prefix already
// names the subsystem, so instead of flattening 50+ historical call sites
// onto General/Error — which made them unfilterable, and buried a wire fault
// among settings-parse noise — route each to its real channel by prefix.
//
// This is a lookup on a string that is a compile-time literal at every call
// site, and it runs only on a path that is already logging (i.e. an error
// that actually happened), so it costs nothing in the common case.
//
// New code should call AGT_LOG directly with an explicit channel and level;
// this exists so the existing swallowed-exception sites gain channel routing
// without 50 mechanical edits that could each introduce a typo.
namespace {

[[nodiscard]] logx::Channel channel_for(std::string_view site) noexcept {
    struct Route { std::string_view prefix; logx::Channel ch; };
    // Order matters only for disjointness; prefixes below are unique.
    static constexpr Route kRoutes[] = {
        {"anthropic.",   logx::Channel::Wire},
        {"openai.",      logx::Channel::Wire},
        {"ollama.",      logx::Channel::Wire},
        {"responses.",   logx::Channel::Wire},
        {"copilot.",     logx::Channel::Wire},
        {"kimi.",        logx::Channel::Wire},
        {"chatgpt.",     logx::Channel::Wire},
        {"modelsdev.",   logx::Channel::Wire},
        {"provider.",    logx::Channel::Wire},
        {"auth.",        logx::Channel::Auth},
        {"oauth.",       logx::Channel::Auth},
        {"keystore.",    logx::Channel::Auth},
        {"accounts.",    logx::Channel::Auth},
        {"persistence.", logx::Channel::Persist},
        {"settings.",    logx::Channel::Persist},
        {"thread.",      logx::Channel::Persist},
        {"memory.",      logx::Channel::Persist},
        {"checkpoint.",  logx::Channel::Persist},
        {"tool.",        logx::Channel::Tool},
        {"tools.",       logx::Channel::Tool},
        {"sandbox.",     logx::Channel::Tool},
        {"hooks.",       logx::Channel::Tool},
        {"skills.",      logx::Channel::Tool},
        {"plugin.",      logx::Channel::Mcp},
        {"mcp.",         logx::Channel::Mcp},
        {"acp.",         logx::Channel::Acp},
        {"rag.",         logx::Channel::Rag},
        {"http.",        logx::Channel::Net},
        {"tls.",         logx::Channel::Net},
        {"net.",         logx::Channel::Net},
        {"smart.",       logx::Channel::Smart},
        {"caps.",        logx::Channel::Model},
        {"salvage.",     logx::Channel::Model},
        {"dispatch.",    logx::Channel::Model},
        {"view.",        logx::Channel::Ui},
        {"render.",      logx::Channel::Ui},
        {"composer.",    logx::Channel::Ui},
        {"clipboard.",   logx::Channel::Ui},
    };
    for (const auto& r : kRoutes)
        if (site.size() >= r.prefix.size()
            && site.compare(0, r.prefix.size(), r.prefix) == 0)
            return r.ch;
    return logx::Channel::General;
}

} // namespace

bool dbglog_enabled() noexcept {
    return logx::enabled(logx::Channel::General, logx::Level::Error);
}

void dbglog(std::string_view where, std::string_view msg) noexcept {
    // Error level: every one of these sites is a swallowed exception or a
    // failed parse — a real fault the program recovered from. Warn+ also
    // enters the crash-time flight recorder, so these show up in a crash
    // dump even when file logging is off.
    logx::emit(channel_for(where), logx::Level::Error, where, msg);
}

} // namespace agentty::util
