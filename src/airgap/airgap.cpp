// `agentty airgap` — one-command launcher for running agentty on an air-gapped
// host through an SSH-tunneled SOCKS5 proxy.  Run from a laptop/box that
// *has* internet to reach an air-gapped host that doesn't:
//
//   agentty airgap user@host                 # connect + run remote agentty
//   agentty airgap --setup user@host         # also: scp ~/.config/agentty/credentials.json -> remote
//
// What it does:
//   - `ssh -R 1080` (port-only form) tells OpenSSH to expose a SOCKS5
//     proxy on the remote at localhost:1080.  Connections to it are
//     tunnelled back through the SSH session and dialed by *this*
//     laptop's OpenSSH.  Net: every TCP destination the remote agentty
//     wants — api.anthropic.com (chat), platform.claude.com (OAuth
//     refresh), arbitrary hosts hit by web_fetch / web_search — reaches
//     the public internet via this laptop, in one tunnel, with no
//     per-host enumeration.
//   - Sets AGENTTY_SOCKS_PROXY=localhost:1080 in the remote shell so the
//     remote agentty routes every dial through that SOCKS proxy instead
//     of trying to resolve hosts directly (which would fail on an
//     air-gapped box).
//   - TLS, cert verification, and the HTTP Host header on the remote
//     stay pinned to the real upstream — the SOCKS proxy can't MITM
//     you.
//   - With --setup: scp the laptop's credentials.json to the remote and
//     chmod 600 it, so the user doesn't have to ferry it manually.
//
// On POSIX the final step exec()s into ssh — signals (Ctrl-C, SIGWINCH)
// reach ssh directly.  On Windows the support is stubbed; OpenSSH client
// is bundled with modern Windows but we leave the orchestration to a
// future contributor with a Win32 box to test on.

#include "agentty/airgap/airgap.hpp"
#include "agentty/util/env.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#else
#  include <spawn.h>
#  include <sys/wait.h>
#  include <unistd.h>
extern char** environ;
#endif

namespace fs = std::filesystem;

namespace agentty::airgap {

namespace {

// Resolve the user's home directory portably: $HOME on POSIX, %USERPROFILE%
// (or %HOMEDRIVE%%HOMEPATH%) on Windows. Empty when none is set.
std::string home_dir() {
    if (const char* h = std::getenv("HOME"); h && *h) return h;
#if defined(_WIN32)
    if (const char* up = std::getenv("USERPROFILE"); up && *up) return up;
    const char* hd = std::getenv("HOMEDRIVE");
    const char* hp = std::getenv("HOMEPATH");
    if (hd && *hd && hp && *hp) return std::string{hd} + hp;
#endif
    return {};
}

void print_usage() {
    std::fprintf(stderr,
        "usage: agentty airgap [--setup] [--remote-agentty PATH] <user@host>\n"
        "       agentty airgap [--setup] <user@host> --acp [acp flags…]\n"
        "\n"
        "  Opens an SSH session to <user@host> with `-R 1080`, which makes\n"
        "  OpenSSH expose a SOCKS5 proxy on the remote's localhost:1080.\n"
        "  Connections to it are tunnelled back through SSH and dialed by\n"
        "  this laptop, so the remote agentty — pointed at the proxy via\n"
        "  AGENTTY_SOCKS_PROXY — can reach every destination it needs (chat,\n"
        "  OAuth refresh, web tools) over a single tunnel.  TLS / cert\n"
        "  verification stay pinned on the real upstream end-to-end, so\n"
        "  the network path between laptop and remote can't MITM you.\n"
        "\n"
        "  Trust boundary (read this before --setup): the remote ends up\n"
        "  with a copy of ~/.config/agentty/credentials.json, which contains\n"
        "  your OAuth refresh token (or API key). A compromised remote\n"
        "  can therefore exfiltrate your Anthropic credentials independent\n"
        "  of the tunnel. agentty airgap protects the *network* between\n"
        "  laptop and remote, not the remote itself — treat the remote as\n"
        "  a credential-bearing peer, not a sandboxed proxy.\n"
        "\n"
        "  --setup            Copy ~/.config/agentty/credentials.json from\n"
        "                     this laptop to the remote (chmod 600) before\n"
        "                     launching.  Run this once on first connect or\n"
        "                     after re-OAuthing locally.\n"
        "  --remote-agentty PATH Absolute path to agentty on the remote.  Default:\n"
        "                     `agentty` (resolved via remote PATH).\n"
        "  --acp [acp flags…] Don't launch the TUI — instead print a ready-to-\n"
        "                     paste Zed `agent_servers` config that runs the\n"
        "                     remote `agentty acp` over this same SSH+SOCKS\n"
        "                     tunnel, with Zed owning the process. Everything\n"
        "                     after --acp is forwarded to the remote acp agent\n"
        "                     (e.g. --acp -m claude-haiku-4-5 --profile ask).\n"
        "                     Must come AFTER <user@host>.\n"
        "  --clipboard-relay  Make Ctrl+V image paste work on the remote.\n"
        "                     Adds a reverse tunnel back to this laptop's\n"
        "                     sshd and points the remote's clipboard reader\n"
        "                     at it, so a pasted image is pulled from THIS\n"
        "                     machine's clipboard on demand.  Requires sshd\n"
        "                     running on the laptop and key-based login to\n"
        "                     localhost (BatchMode — no password prompt).\n"
        "\n"
        "  ssh and scp must be on this laptop's PATH.  Pass extra ssh args\n"
        "  via the AGENTTY_AIRGAP_SSH env var (e.g. -i, -p, -J).\n"
        "\n"
        "  Image paste (Ctrl+V) on the remote: easiest is --clipboard-relay\n"
        "  above.  For full control, set AGENTTY_CLIPBOARD_CMD on the remote\n"
        "  (or on this laptop — it's forwarded) to any command that prints\n"
        "  the laptop's clipboard image to stdout, e.g.:\n"
        "    AGENTTY_CLIPBOARD_CMD='ssh laptop wl-paste --type image/png'\n"
        "  Without either, attach images by path instead.\n");
}

// Synchronously spawn `argv[0]` with the given argv and wait for it.
// Inherits stdin/stdout/stderr.  Returns the child's exit code, or -1 on
// spawn failure. PATH-resolved on both platforms.
#if defined(_WIN32)
// Quote one token per the MSVCRT/CommandLineToArgvW rules so the child
// recovers the exact argument.
std::string win_quote_arg(const std::string& a) {
    if (!a.empty() && a.find_first_of(" \t\n\v\"") == std::string::npos)
        return a;
    std::string out = "\"";
    for (auto it = a.begin();; ++it) {
        std::size_t bs = 0;
        while (it != a.end() && *it == '\\') { ++it; ++bs; }
        if (it == a.end()) { out.append(bs * 2, '\\'); break; }
        else if (*it == '"') { out.append(bs * 2 + 1, '\\'); out.push_back('"'); }
        else { out.append(bs, '\\'); out.push_back(*it); }
    }
    out.push_back('"');
    return out;
}

int run_sync(const std::vector<std::string>& argv) {
    if (argv.empty()) return -1;
    std::string cmdline = win_quote_arg(argv[0]);
    for (std::size_t i = 1; i < argv.size(); ++i) {
        cmdline.push_back(' ');
        cmdline += win_quote_arg(argv[i]);
    }

    STARTUPINFOA si{};
    si.cb      = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput  = ::GetStdHandle(STD_INPUT_HANDLE);
    si.hStdOutput = ::GetStdHandle(STD_OUTPUT_HANDLE);
    si.hStdError  = ::GetStdHandle(STD_ERROR_HANDLE);

    PROCESS_INFORMATION pi{};
    // lpApplicationName=nullptr → CreateProcess does PATH + .exe resolution
    // on the first cmdline token, matching execvp/posix_spawnp semantics.
    BOOL ok = ::CreateProcessA(nullptr, cmdline.data(), nullptr, nullptr,
                               /*bInheritHandles=*/TRUE, 0, nullptr, nullptr,
                               &si, &pi);
    if (!ok) {
        std::fprintf(stderr,
            "agentty airgap: failed to spawn `%s` (CreateProcess err %lu)\n",
            argv[0].c_str(), static_cast<unsigned long>(::GetLastError()));
        return -1;
    }
    ::CloseHandle(pi.hThread);
    ::WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 0;
    ::GetExitCodeProcess(pi.hProcess, &code);
    ::CloseHandle(pi.hProcess);
    return static_cast<int>(code);
}
#else
int run_sync(const std::vector<std::string>& argv) {
    if (argv.empty()) return -1;

    // posix_spawn wants `char* const*`; build a contiguous argv buffer
    // from the std::strings without const-casting their internals.
    std::vector<char*> raw;
    raw.reserve(argv.size() + 1);
    for (const auto& s : argv) raw.push_back(const_cast<char*>(s.c_str()));
    raw.push_back(nullptr);

    pid_t pid = -1;
    int rc = ::posix_spawnp(&pid, raw[0], /*file_actions=*/nullptr,
                            /*attr=*/nullptr, raw.data(), environ);
    if (rc != 0) {
        std::fprintf(stderr,
            "agentty airgap: failed to spawn `%s`: %s\n",
            raw[0], std::strerror(rc));
        return -1;
    }

    int status = 0;
    while (::waitpid(pid, &status, 0) == -1) {
        if (errno == EINTR) continue;
        std::fprintf(stderr,
            "agentty airgap: waitpid failed: %s\n", std::strerror(errno));
        return -1;
    }
    if (WIFEXITED(status))   return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
    return -1;
}
#endif

// Copy ~/.config/agentty/credentials.json from this laptop to the remote.
// Three steps: ensure the remote directory exists, scp the file, fix
// perms.  Each step is run synchronously; we abort on the first failure
// so the user sees exactly which step blew up.
int copy_credentials(const std::string& remote) {
    std::string home = home_dir();
    if (home.empty()) {
        std::fprintf(stderr, "agentty airgap: HOME is unset.\n");
        return 1;
    }
    fs::path local = fs::path{home} / ".config" / "agentty" / "credentials.json";
    std::error_code ec;
    if (!fs::exists(local, ec) || ec) {
        std::fprintf(stderr,
            "agentty airgap: no local credentials at %s\n"
            "             run `agentty login` on this machine first.\n",
            local.string().c_str());
        return 1;
    }

    std::fprintf(stderr, "agentty airgap: copying credentials -> %s …\n",
                 remote.c_str());

    // 1) mkdir + chmod 700 for the remote config dir.  -p is idempotent.
    if (int rc = run_sync({
            "ssh", remote,
            "mkdir -p ~/.config/agentty && chmod 700 ~/.config/agentty",
        }); rc != 0) {
        std::fprintf(stderr,
            "agentty airgap: remote mkdir failed (ssh exit %d).\n", rc);
        return rc < 0 ? 1 : rc;
    }

    // 2) scp the credentials file.
    {
        std::string dest = remote + ":.config/agentty/credentials.json";
        if (int rc = run_sync({"scp", "-q", local.string(), dest});
            rc != 0) {
            std::fprintf(stderr,
                "agentty airgap: scp failed (exit %d).\n", rc);
            return rc < 0 ? 1 : rc;
        }
    }

    // 3) chmod 600 — scp doesn't reliably preserve mode across hosts and
    //    the loader expects 0600 for the OAuth token.
    if (int rc = run_sync({
            "ssh", remote,
            "chmod 600 ~/.config/agentty/credentials.json",
        }); rc != 0) {
        std::fprintf(stderr,
            "agentty airgap: remote chmod failed (ssh exit %d).\n", rc);
        return rc < 0 ? 1 : rc;
    }

    std::fprintf(stderr, "agentty airgap: credentials copied.\n");
    return 0;
}

// Single-quote `v` so it survives the remote shell as one literal token.
// Embedded `'` becomes `'\''` (close-quote, escaped quote, reopen-quote).
std::string sh_squote(std::string_view v) {
    std::string out;
    out.reserve(v.size() + 2);
    out.push_back('\'');
    for (char c : v) {
        if (c == '\'') out.append("'\\''");
        else           out.push_back(c);
    }
    out.push_back('\'');
    return out;
}

// Replace this process with `ssh -t -R 1080 user@host <remote-cmd>`.
// Never returns on success.  argv constructed on the heap because execvp
// modifies its argv slot.
//
// clipboard_relay: when true, also open a reverse tunnel
// `-R <kRelayPort>:localhost:22` so the air-gapped remote can ssh back
// to *this* laptop's sshd, and set AGENTTY_CLIPBOARD_CMD on the remote
// to a command that runs the laptop's clipboard reader through it. This
// is what makes Ctrl+V image paste work in airgap: the bytes are pulled
// from the laptop's clipboard on demand over the existing session.
[[noreturn]] void exec_ssh(const std::string& remote,
                           const std::string& remote_agentty,
                           bool clipboard_relay) {
    // Reverse-tunnel port for the clipboard relay: the remote's
    // localhost:kRelayPort forwards back to this laptop's sshd (:22),
    // so the synthesized AGENTTY_CLIPBOARD_CMD can ssh home and read
    // the laptop clipboard. Distinct from the SOCKS port (1080).
    constexpr int kRelayPort = 1175;
    // Remote command: point agentty at the tunnelled SOCKS5 proxy, exec it.
    // `exec` matters — without it the user's $SHELL stays around as a
    // parent process and signal forwarding gets one extra hop wrong.
    //
    // MAYA_FORCE_SYNC=1: force maya to emit DEC 2026 (begin-sync-update)
    // around every inline frame.  We also forward terminal-identifying
    // env vars below so maya's auto-detect can fire, but plenty of
    // common terminals (bare xterm, st, urxvt, custom builds) leave no
    // identifying marker and fall through env-detect even though they
    // honor 2026 just fine.  Modern terminals silently ignore unknown
    // private CSIs, so forcing it here is no-op on the unlucky few and
    // a cure for the visible "new turn appears at the bottom then
    // realigns" flicker on the rest.
    std::string remote_cmd = "AGENTTY_SOCKS_PROXY=localhost:1080 MAYA_FORCE_SYNC=1";

    // ---- Clipboard image relay ---------------------------------------
    // The air-gapped remote has no clipboard of its own, so Ctrl+V image
    // paste needs the bytes pulled from *this* laptop's clipboard on
    // demand. AGENTTY_CLIPBOARD_CMD on the remote is the hook; two ways
    // to populate it:
    //   1. User set it explicitly on the laptop -> forward verbatim.
    //   2. --clipboard-relay -> we add a reverse tunnel back to the
    //      laptop's sshd (see kRelayPort below) and synthesize a command
    //      that runs the laptop's clipboard reader (wl-paste / xclip)
    //      through it.
    // (1) wins if both are present.
    if (const char* cc =
            util::env::get_or_null<util::env::Var::ClipboardCmd>()) {
        remote_cmd += " AGENTTY_CLIPBOARD_CMD=";
        remote_cmd += sh_squote(cc);
    } else if (clipboard_relay) {
        // Pick the laptop's clipboard reader from its session type:
        // wl-paste on Wayland, xclip on X11. We request image/png —
        // sniff_image_type on the remote validates whatever returns.
        bool laptop_wayland = false;
        if (const char* st = std::getenv("XDG_SESSION_TYPE"))
            laptop_wayland = std::string_view{st} == "wayland";
        if (const char* w = std::getenv("WAYLAND_DISPLAY"); w && *w)
            laptop_wayland = true;

        // A non-interactive `ssh host cmd` does NOT inherit the laptop's
        // graphical-session env, so wl-paste/xclip can't find the
        // display. We captured WAYLAND_DISPLAY / DISPLAY here (on the
        // laptop, where they're set) and prepend them to the callback
        // command so the reader connects to the right compositor.
        std::string disp_prefix;
        if (laptop_wayland) {
            if (const char* w = std::getenv("WAYLAND_DISPLAY"); w && *w)
                disp_prefix += "WAYLAND_DISPLAY=" + std::string{w} + " ";
        } else if (const char* d = std::getenv("DISPLAY"); d && *d) {
            disp_prefix += "DISPLAY=" + std::string{d} + " ";
        }
        const char* reader = laptop_wayland
            ? "wl-paste --type image/png"
            : "xclip -selection clipboard -t image/png -o";

        const char* user = std::getenv("USER");
        std::string dest = (user && *user)
            ? std::string{user} + "@localhost" : std::string{"localhost"};
        // BatchMode=yes so a missing key fails fast instead of prompting
        // for a password (the paste runs unattended on Ctrl+V).
        std::string back =
            "ssh -p " + std::to_string(kRelayPort)
            + " -o StrictHostKeyChecking=no"
              " -o UserKnownHostsFile=/dev/null"
              " -o BatchMode=yes"
              " -o LogLevel=ERROR "
            + dest + " " + disp_prefix + reader;
        remote_cmd += " AGENTTY_CLIPBOARD_CMD=";
        remote_cmd += sh_squote(back);
    }


    // Forward terminal-identifying env vars from this laptop to the
    // remote shell so the remote agentty can tell whether the user's
    // terminal supports DEC 2026 (synchronized output / "begin sync
    // update").  Without sync, compose_inline_frame's per-cell rewrite
    // is rendered byte-by-byte and the user sees flicker every time the
    // composer's row gets repainted.  SSH doesn't propagate these vars
    // by default — `SendEnv` requires the server's sshd_config to
    // explicitly `AcceptEnv` each one — so we pass them on the command
    // line where no sshd config change is needed.  If a var is unset
    // locally, we skip it; an empty value would set the env var to ""
    // on the remote, which env-detect treats as "absent" anyway.
    static constexpr const char* kTerminalMarkers[] = {
        // Identification by program name / version.
        "TERM_PROGRAM", "TERM_PROGRAM_VERSION",
        // Per-terminal unambiguous markers.
        "KITTY_WINDOW_ID",
        "ALACRITTY_LOG", "ALACRITTY_WINDOW_ID",
        "GHOSTTY_RESOURCES_DIR",
        "WEZTERM_EXECUTABLE",
        "WT_SESSION",          // Windows Terminal
        "KONSOLE_VERSION",
        "VTE_VERSION",          // GNOME Terminal, Tilix, etc.
        "ITERM_SESSION_ID",     // iTerm.app
        // TERM is typically already propagated by SSH, but force it in
        // case the user's sshd_config doesn't pass it through.
        "TERM",
        // COLORTERM signals truecolor support — useful for agentty's
        // colour rendering.
        "COLORTERM",
    };
    for (const char* name : kTerminalMarkers) {
        if (const char* v = std::getenv(name); v && *v) {
            remote_cmd += ' ';
            remote_cmd += name;
            remote_cmd += '=';
            remote_cmd += sh_squote(v);
        }
    }

    remote_cmd += " exec " + remote_agentty;

    std::vector<std::string> argv;
    argv.push_back("ssh");
    argv.push_back("-t");                       // agentty is interactive
    // -R <port> with no `host:hostport` tail tells OpenSSH to expose a
    // SOCKS proxy on the remote's <port> and resolve+dial connections
    // it receives via *this* (laptop) side.  Requires OpenSSH ≥ 7.6 on
    // both ends — practically every modern install.
    argv.push_back("-R"); argv.push_back("1080");

    // Clipboard relay reverse tunnel: remote localhost:kRelayPort ->
    // this laptop's sshd. Only added with --clipboard-relay; lets the
    // remote's AGENTTY_CLIPBOARD_CMD ssh home to read the clipboard.
    // -A forwards this laptop's ssh-agent so the callback authenticates
    // back to localhost with your key without copying one to the remote.
    if (clipboard_relay) {
        argv.push_back("-A");
        argv.push_back("-R");
        argv.push_back(std::to_string(kRelayPort) + ":localhost:22");
    }

    // Liveness defaults.  All passed *before* AGENTTY_AIRGAP_SSH so a user-
    // supplied `-o Foo=bar` later silently wins (OpenSSH applies the last
    // `-o` for any given key).
    //
    //   ServerAliveInterval=30   Carrier-TCP keepalive on the SSH session —
    //                            keeps the channel from going stale during
    //                            quiet stretches between turns.
    //   ServerAliveCountMax=3    Tolerate 3 missed keepalives before
    //                            tearing down.
    //   TCPKeepAlive=yes         Kernel-level TCP keepalive on the carrier.
    //   ConnectTimeout=10        Bound the initial TCP connect so a flaky
    //                            network doesn't hang the user for 2 min.
    //   ExitOnForwardFailure=yes Bail loudly if the -R 1080 reverse listener
    //                            can't bind on the remote (something else
    //                            already on 1080), instead of silently
    //                            exposing no SOCKS proxy and letting agentty
    //                            fail later with a confusing getaddrinfo.
    //
    // Compression intentionally NOT enabled by default.  Inline-mode frames
    // are small bursty deltas (~200-2 KiB per render); zlib is tuned for
    // bulk and adds latency to small chunks because the receiver waits for
    // sync markers — perceived UI lag was the dominant complaint.  Bulk
    // file payloads (web_fetch, large bash outputs) compress less than the
    // UI frames lose, on net.  Users on bandwidth-constrained links can
    // opt in: `AGENTTY_AIRGAP_SSH="-o Compression=yes" agentty airgap …`.
    auto add_o = [&](const char* spec) {
        argv.push_back("-o"); argv.push_back(spec);
    };
    add_o("ServerAliveInterval=30");
    add_o("ServerAliveCountMax=3");
    add_o("TCPKeepAlive=yes");
    add_o("ConnectTimeout=10");
    add_o("ExitOnForwardFailure=yes");

    // AGENTTY_AIRGAP_SSH lets the user inject extra flags (`-i`, `-p`, `-J`,
    // or override any of the defaults above with a later `-o ...`).
    // We split on whitespace — primitive but enough for the common case.
    if (const char* extra = util::env::get_or_null<util::env::Var::AirgapSsh>()) {
        std::string buf;
        for (const char* p = extra; ; ++p) {
            if (*p == ' ' || *p == '\t' || *p == '\0') {
                if (!buf.empty()) { argv.push_back(std::move(buf)); buf.clear(); }
                if (*p == '\0') break;
            } else {
                buf.push_back(*p);
            }
        }
    }

    argv.push_back(remote);
    argv.push_back(std::move(remote_cmd));

    // Spawn ssh, inherit our stdio (so the remote TUI drives this terminal),
    // wait, and exit with its code. We used to execvp() to replace this
    // process, but spawn-and-forward-exit is identical from the user's shell
    // and works on Windows too (no execvp there). PATH-resolves `ssh` on both
    // platforms (OpenSSH ships with Windows 10+).
    int rc = run_sync(argv);
    if (rc < 0) {
        std::fprintf(stderr,
            "agentty airgap: failed to run `ssh`.\n"
            "             ensure the OpenSSH client is installed and on PATH.\n");
        std::exit(1);
    }
    std::exit(rc);
}

// Emit a ready-to-paste Zed `agent_servers` config that makes Zed itself
// own the airgap tunnel: `command` is `ssh`, and its args open the reverse
// SOCKS tunnel AND exec `agentty acp` on the remote in one invocation. The
// ACP JSON-RPC rides ssh's stdio, so there's nothing to babysit — no separate
// `ssh -N`, no remote wrapper script, no env block. Zed spawns it, Zed kills
// it. This is the whole point of `--acp`: copy-paste and go.
//
// `acp_extra` is the agentty-acp tail the user passed after `--acp`
// (e.g. "-m claude-haiku-4-5 --profile ask --workspace /"). It's appended
// verbatim to the remote `agentty acp` command.
void print_acp_config(const std::string& remote,
                      const std::string& remote_agentty,
                      const std::string& acp_extra) {
    // Remote command: point agentty at the tunnelled SOCKS proxy, then exec
    // the ACP agent. `exec` so signals reach agentty directly and ssh tears
    // it down cleanly when Zed closes stdin.
    std::string remote_cmd =
        "AGENTTY_SOCKS_PROXY=localhost:1080 exec " + remote_agentty + " acp";
    if (!acp_extra.empty()) remote_cmd += " " + acp_extra;

    // The ssh args, as a JSON array. -T: no PTY (ACP is a byte protocol, a
    // PTY would corrupt it with echo/CR translation). -R 1080: the reverse
    // SOCKS proxy. Liveness opts keep the long-lived session up.
    auto jstr = [](const std::string& s) {
        std::string o = "\"";
        for (char c : s) { if (c == '\\' || c == '"') o += '\\'; o += c; }
        o += '"';
        return o;
    };
    const std::vector<std::string> ssh_args = {
        "-T",
        "-R", "1080",
        "-o", "ExitOnForwardFailure=yes",
        "-o", "ServerAliveInterval=30",
        "-o", "ServerAliveCountMax=3",
        remote,
        remote_cmd,
    };

    std::string args_json;
    for (std::size_t i = 0; i < ssh_args.size(); ++i) {
        if (i) args_json += ", ";
        args_json += jstr(ssh_args[i]);
    }

    // Best-effort: tell the user exactly which file to paste into.
    std::string settings_hint;
#if defined(_WIN32)
    if (const char* appdata = std::getenv("APPDATA"); appdata && *appdata)
        settings_hint = (fs::path{appdata} / "Zed" / "settings.json").string();
#else
    if (std::string home = home_dir(); !home.empty())
        settings_hint = (fs::path{home} / ".config" / "zed" / "settings.json").string();
#endif

    std::fprintf(stderr,
        "agentty airgap --acp: add this to Zed's settings.json%s%s\n"
        "  (the laptop's settings — NOT the remote's; Zed runs `ssh` locally\n"
        "   and the agent over its stdio, so this works for a LOCAL Zed\n"
        "   project too, not just an SSH-remote one):\n"
        "\n"
        "  \"agent_servers\": {\n"
        "    \"agentty (airgap)\": {\n"
        "      \"command\": \"ssh\",\n"
        "      \"args\": [%s]\n"
        "    }\n"
        "  }\n"
        "\n"
        "Then pick “agentty (airgap)” in Zed's agent panel. One ssh process is\n"
        "the tunnel, the agent, and the transport — nothing to keep running by\n"
        "hand. Make sure `%s` has agentty installed and its credentials\n"
        "(run `agentty airgap --setup %s` once to copy them over).\n",
        settings_hint.empty() ? "" : "\n  → ",
        settings_hint.c_str(),
        args_json.c_str(), remote.c_str(), remote.c_str());
}

} // namespace

int cmd_airgap(int argc, char** argv) {
    bool        setup_mode = false;
    bool        clipboard_relay = false;
    bool        acp_mode = false;
    std::string remote_agentty = "agentty";
    std::string remote;
    std::string acp_extra;   // everything after --acp, forwarded to `agentty acp`

    for (int i = 0; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "-h" || a == "--help") { print_usage(); return 0; }
        else if (a == "--setup")       { setup_mode = true; }
        else if (a == "--clipboard-relay") { clipboard_relay = true; }
        else if (a == "--acp") {
            // Everything after --acp is the agentty-acp flag tail (model,
            // profile, workspace, sandbox …). Slurp the rest verbatim and
            // stop parsing our own flags.
            acp_mode = true;
            for (int j = i + 1; j < argc; ++j) {
                if (!acp_extra.empty()) acp_extra += ' ';
                acp_extra += argv[j];
            }
            break;
        }
        else if (a == "--remote-agentty" && i + 1 < argc) {
            remote_agentty = argv[++i];
        }
        else if (!a.empty() && a[0] != '-' && remote.empty()) {
            remote = std::move(a);
        }
        else {
            std::fprintf(stderr, "agentty airgap: unrecognized argument: %s\n\n",
                         a.c_str());
            print_usage();
            return 64;
        }
    }

    if (remote.empty()) { print_usage(); return 64; }

    if (setup_mode) {
        if (int rc = copy_credentials(remote); rc != 0) return rc;
    }

    // --acp doesn't launch anything itself: it prints the Zed config that
    // wires ssh-as-agent-server, then exits. Zed owns the process lifecycle
    // from there.
    if (acp_mode) {
        print_acp_config(remote, remote_agentty, acp_extra);
        return 0;
    }

    if (clipboard_relay) {
        // The relay's callback authenticates back to localhost via the
        // forwarded ssh-agent (BatchMode, no password prompt). Without
        // an agent holding a key the laptop accepts, the callback fails
        // silently and Ctrl+V just reports an empty clipboard — warn now.
        const char* sock = std::getenv("SSH_AUTH_SOCK");
        if (!sock || !*sock) {
            std::fprintf(stderr,
                "agentty airgap: --clipboard-relay needs a running ssh-agent "
                "on this laptop\n"
                "             (the remote authenticates back via forwarded "
                "agent). Start one and add your key:\n"
                "               eval \"$(ssh-agent -s)\" && ssh-add\n"
                "             then re-run. Continuing anyway — image paste "
                "may not work.\n");
        }
    }

    exec_ssh(remote, remote_agentty, clipboard_relay);  // [[noreturn]]
}

} // namespace agentty::airgap
