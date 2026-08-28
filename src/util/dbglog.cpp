#include "agentty/util/dbglog.hpp"

#include "agentty/util/logx.hpp"

namespace agentty::util {

// dbglog is now a thin COMPAT SHIM over logx (the structured log).
// AGENTTY_DEBUG_LOG keeps working: logx's init treats it as
// AGENTTY_LOG_FILE + a `debug` default filter. Every existing catch-site
// call lands on the `general` channel at Error level — which also means
// swallowed exceptions now enter the crash-dump flight recorder even when
// file logging is off.

bool dbglog_enabled() noexcept {
    return logx::enabled(logx::Channel::General, logx::Level::Error);
}

void dbglog(std::string_view where, std::string_view msg) noexcept {
    logx::emit(logx::Channel::General, logx::Level::Error, where, msg);
}

} // namespace agentty::util
