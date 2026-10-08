// cmd::diagnose_clipboard — why did a clipboard read go unanswered?
//
// Answering that walks /proc (is mosh-server one of our ancestors, or of the
// attached tmux client?) and asks the tmux server about its config, so it
// runs on a worker and replies with ClipboardDiagnosed. The reducer only
// shows the message.

#include "agentty/runtime/app/cmd_factory.hpp"

#include <string>

#if defined(__linux__)
#include <fstream>
#include <sstream>
#include <unistd.h>
#endif

#include <maya/terminal/tmux.hpp>

namespace agentty::app::cmd {

namespace {

#if defined(__linux__)
// Walk up from `pid` through /proc looking for mosh-server. mosh exports no
// reliable env marker into its child shell, so ancestry is the only signal.
bool mosh_above(int pid) {
    for (int hop = 0; hop < 12 && pid > 1; ++hop) {
        const std::string base = "/proc/" + std::to_string(pid);
        if (std::ifstream comm(base + "/comm"); comm) {
            std::string name;
            std::getline(comm, name);
            if (name.find("mosh-server") != std::string::npos) return true;
        }
        std::ifstream stat(base + "/stat");
        if (!stat) break;
        // stat: pid (comm) state ppid ... — comm may contain spaces, so
        // parse from after the LAST ')'.
        std::string line;
        std::getline(stat, line);
        const auto rp = line.rfind(')');
        if (rp == std::string::npos) break;
        std::istringstream tail(line.substr(rp + 2));
        char state = 0;
        int ppid = 0;
        tail >> state >> ppid;
        if (ppid <= 1) break;
        pid = ppid;
    }
    return false;
}
#endif

bool mosh_session() {
#if defined(__linux__)
    if (mosh_above(static_cast<int>(::getppid()))) return true;
    // Ancestry misses persistent tmux: when the tmux server was started by
    // systemd or an older login, mosh-server is a sibling of the server, not
    // our ancestor. Walk the attached client instead.
    if (maya::tmux::active())
        if (const int cpid = maya::tmux::client_pid(); cpid > 1)
            return mosh_above(cpid);
#endif
    return false;
}

std::string diagnose(bool in_ssh) {
    const bool in_mosh = mosh_session();
    const bool in_tmux = maya::tmux::active();
    // tmux capabilities are per-client and maya memoises the probe, so a
    // reattach from another terminal leaves a stale verdict. A read just
    // failed, which is worth one round-trip to re-check.
    if (in_tmux) (void)maya::tmux::refresh_if_client_changed();
    std::string msg;
    if (in_mosh) {
        msg = "clipboard: mosh doesn't relay terminal clipboard "
              "replies \xe2\x80\x94 use plain ssh (or tmux over ssh), or set "
              "AGENTTY_CLIPBOARD_CMD";
    } else if (in_tmux) {
        // Name the ACTUAL blocker instead of guessing. Order
        // matters: these are checked in the order tmux applies
        // them, so the first failure reported is the first one
        // the user has to fix.
        if (!maya::tmux::clipboard_reads_relayed()) {
            // THE common case, and silent until now: tmux's
            // `get-clipboard` defaults to `buffer`, so tmux
            // answers a clipboard read from its OWN paste buffer
            // and never asks the terminal. A paste buffer holds
            // TEXT — an image can never come back through it, no
            // matter what the outer terminal supports.
            msg = "clipboard: tmux answers reads from its own paste "
                  "buffer (text only) \xe2\x80\x94 run `tmux set -g "
                  "get-clipboard both` so it asks your terminal, "
                  "then retry";
        } else if (!maya::tmux::passthrough_allowed()) {
            msg = "clipboard: tmux is dropping the request \xe2\x80\x94 run "
                  "`tmux set -g allow-passthrough on` (it is OFF by "
                  "default), then retry";
        } else if (!maya::tmux::has_feature(
                       maya::tmux::Feature::Clipboard)) {
            msg = "clipboard: tmux reports your outer terminal has no "
                  "clipboard support \xe2\x80\x94 add `set -ga terminal-features "
                  "\",*:clipboard\"` if it does, or set AGENTTY_CLIPBOARD_CMD";
        } else {
            msg = "clipboard: no reply through tmux \xe2\x80\x94 passthrough and "
                  "clipboard are on, so the outer terminal refused the "
                  "read (in kitty: add read-clipboard to clipboard_control "
                  "in kitty.conf and restart kitty)";
        }
    } else if (in_ssh) {
        msg = "clipboard: your terminal didn't answer \xe2\x80\x94 images "
              "over SSH need kitty with clipboard_control read-clipboard "
              "in kitty.conf (restart kitty after adding it); else set "
              "AGENTTY_CLIPBOARD_CMD='ssh <laptop> wl-paste -t image/png'";
    } else {
        msg = "clipboard: terminal didn't answer the read query "
              "\xe2\x80\x94 install wl-clipboard/xclip (Linux) or use a "
              "terminal with OSC 52 read support";
    }
    return msg;
}

}  // namespace

Cmd diagnose_clipboard(bool in_ssh) {
    return Cmd::task(
        [](jaal::Sink<Msg> out, std::stop_token, bool in_ssh) {
            std::string message;
            try {
                message = diagnose(in_ssh);
            } catch (...) {
                message = "clipboard: no reply from the terminal";
            }
            out.send(Msg{msg::ComposerMsg{ClipboardDiagnosed{std::move(message)}}});
        },
        in_ssh);
}

}  // namespace agentty::app::cmd
