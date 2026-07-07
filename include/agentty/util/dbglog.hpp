#pragma once
// agentty::util::dbglog — opt-in diagnostic log for the swallowed-error
// paths (SECURITY_AUDIT-adjacent: silent `catch(...) {}` sites hide
// corruption and make field bugs unreproducible).
//
// agentty owns the terminal, so a bare fprintf(stderr, ...) would scribble
// over the TUI. Instead this appends a timestamped line to the file named
// by the AGENTTY_DEBUG_LOG env var. When the var is unset (the default for
// every normal run) the call is a couple of cheap loads and returns — no
// file is opened, no formatting happens, zero behaviour change.
//
// Usage at a previously-silent catch site:
//     } catch (const std::exception& e) {
//         util::dbglog("persistence.save", e.what());
//     } catch (...) {
//         util::dbglog("persistence.save", "non-std exception");
//     }
//
// The point is diagnosability, NOT changing control flow: every site keeps
// its existing best-effort recovery. We just stop throwing the "what
// happened" away.

#include <exception>
#include <string_view>

namespace agentty::util {

// True iff AGENTTY_DEBUG_LOG is set to a non-empty path. Cached after the
// first call. Lets a hot catch site skip building a message string when
// logging is off: `if (dbglog_enabled()) dbglog(where, expensive())`.
bool dbglog_enabled() noexcept;

// Append "<ISO-ish timestamp> [where] msg\n" to $AGENTTY_DEBUG_LOG.
// No-op (and does not touch the filesystem) when logging is disabled.
// Thread-safe: serialized by an internal mutex.
void dbglog(std::string_view where, std::string_view msg) noexcept;

// Convenience for catch blocks: logs `e.what()` under `where`. Pair with a
// trailing `catch (...) { dbglog(where, "non-std exception"); }` for the
// non-std arm.
inline void dbglog_ex(std::string_view where, const std::exception& e) noexcept {
    dbglog(where, e.what());
}

} // namespace agentty::util
