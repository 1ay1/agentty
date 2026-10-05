#include "agentty/io/clipboard.hpp"
#include "agentty/util/env.hpp"
#include "agentty/util/logx.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#if !defined(_WIN32)
#  include <unistd.h>   // getpid (Kitty clipboard temp path), access/X_OK (tool_in_path)
#endif

#if defined(_WIN32)
  #define AGENTTY_POPEN  ::_popen
  #define AGENTTY_PCLOSE ::_pclose
  #define AGENTTY_POPEN_MODE "rb"

  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  #ifndef NOMINMAX
    #define NOMINMAX
  #endif
  #include <windows.h>
  #include <objidl.h>     // IStream
  #include <shlwapi.h>    // SHCreateMemStream
  #include <gdiplus.h>
#else
  #define AGENTTY_POPEN  ::popen
  #define AGENTTY_PCLOSE ::pclose
  #define AGENTTY_POPEN_MODE "r"
#endif

namespace agentty {

namespace {

// Anthropic's per-image cap is 5 MB; raw 8 MiB after base64 expansion
// is the most we'd ship. Bigger captures get truncated to 8 MiB at
// the read boundary — magic-byte sniff still works on the prefix.
constexpr std::size_t kCap = 8 * 1024 * 1024;

// CaptureResult is used by every platform — wrap() takes one, the POSIX
// helpers populate it via popen, and the Win32 path constructs it
// directly from clipboard bytes. Keep it outside the platform guard.
struct CaptureResult {
    std::string bytes;
    int         status = -1;
};

// sniff_image_type is used by wrap() on every platform — keep it
// outside the POSIX guard.
const char* sniff_image_type(std::string_view bytes) {
    auto u = [&](std::size_t i){ return static_cast<unsigned char>(bytes[i]); };
    if (bytes.size() >= 8 && u(0) == 0x89 && u(1) == 0x50 && u(2) == 0x4E
        && u(3) == 0x47 && u(4) == 0x0D && u(5) == 0x0A && u(6) == 0x1A
        && u(7) == 0x0A) return "image/png";
    if (bytes.size() >= 3 && u(0) == 0xFF && u(1) == 0xD8 && u(2) == 0xFF)
        return "image/jpeg";
    if (bytes.size() >= 6
        && bytes[0] == 'G' && bytes[1] == 'I' && bytes[2] == 'F'
        && bytes[3] == '8' && (bytes[4] == '7' || bytes[4] == '9')
        && bytes[5] == 'a') return "image/gif";
    if (bytes.size() >= 12
        && bytes[0] == 'R' && bytes[1] == 'I' && bytes[2] == 'F'
        && bytes[3] == 'F' && bytes[8] == 'W' && bytes[9] == 'E'
        && bytes[10] == 'B' && bytes[11] == 'P') return "image/webp";
    return nullptr;
}

// Run a shell command and capture binary stdout up to `cap` bytes.
// Returns the bytes plus the wait-status. status==-1 means popen
// failed outright (rare — fork/exec issues). Available on every
// platform: Windows maps AGENTTY_POPEN → _popen at the top of the
// file, so the AGENTTY_CLIPBOARD_CMD override works there too.
CaptureResult popen_capture(const char* cmd, std::size_t cap) {
    CaptureResult r;
    FILE* fp = AGENTTY_POPEN(cmd, AGENTTY_POPEN_MODE);
    if (!fp) return r;
    r.bytes.reserve(std::min<std::size_t>(cap, 256 * 1024));
    char buf[8192];
    while (r.bytes.size() < cap) {
        std::size_t avail = cap - r.bytes.size();
        std::size_t want  = avail < sizeof(buf) ? avail : sizeof(buf);
        std::size_t n = std::fread(buf, 1, want, fp);
        if (n == 0) break;
        r.bytes.append(buf, n);
    }
    r.status = AGENTTY_PCLOSE(fp);
    return r;
}

// Image capture via a user-supplied command (AGENTTY_CLIPBOARD_CMD).
// Defined after wrap() below (it depends on it).

// The tool-detection helpers below are only used on POSIX / macOS. On
// Windows the clipboard read goes through the Win32 API directly and
// these would trip -Wunused-function.
#if !defined(_WIN32)

// Is `name` an executable on PATH?
//
// This used to be `std::system("command -v " + name)`, which is a shell
// sink: every caller today passes a string literal (wl-paste, xclip,
// pngpaste, …) so it was never live, but it sat one careless caller away
// from command injection — a name containing `;` or `$(…)` would execute.
// Walking PATH with access(X_OK) removes the shell entirely, and as a
// bonus skips a fork+exec of /bin/sh per probe. A name containing a
// slash is rejected outright: PATH lookup is not meaningful for it, and
// accepting one would let a relative path escape the intended search.
bool tool_in_path(const char* name) {
    if (!name || !*name) return false;
    const std::string_view n{name};
    if (n.find('/') != std::string_view::npos) return false;

    const char* path = std::getenv("PATH");
    if (!path || !*path) return false;

    std::string_view rest{path};
    while (!rest.empty()) {
        const auto colon = rest.find(':');
        std::string_view dir = rest.substr(0, colon);
        rest = (colon == std::string_view::npos)
                 ? std::string_view{} : rest.substr(colon + 1);
        // A literal empty entry in PATH means "." by POSIX convention.
        std::string cand{dir.empty() ? std::string_view{"."} : dir};
        if (cand.back() != '/') cand += '/';
        cand += n;
        if (::access(cand.c_str(), X_OK) == 0) return true;
    }
    return false;
}

// Pick the best image-class MIME type from a newline-separated
// listing the clipboard advertises. Preference order matches the
// quality that survives a round trip through Anthropic's image
// resizing: lossless first (PNG / WEBP-lossless), then lossy. Note
// that some Qt apps publish only `application/x-qt-image`, which
// looks image-y but doesn't actually carry decodable PNG/JPEG bytes
// — we explicitly skip it.
const char* pick_clipboard_image_type(std::string_view types) {
    static const char* prefs[] = {
        "image/png",
        "image/jpeg", "image/jpg",
        "image/webp",
        "image/gif",
        "image/bmp",
        "image/tiff",
    };
    for (const char* p : prefs) {
        if (types.find(p) != std::string_view::npos) return p;
    }
    return nullptr;
}

[[maybe_unused]] bool clipboard_has_qt_image_only(std::string_view types) {
    if (types.find("application/x-qt-image") == std::string_view::npos)
        return false;
    return pick_clipboard_image_type(types) == nullptr;
}

#if defined(__linux__)

// ---------------------------------------------------------------------------
// Display-server discovery that does NOT trust the environment
// ---------------------------------------------------------------------------
//
// WAYLAND_DISPLAY / XDG_SESSION_TYPE are the obvious way to find the
// compositor, and they are wrong often enough to be the single biggest
// source of "image paste stopped working" reports. The reason is process
// ancestry: agentty inherits the environment of whatever started it, and
// long-lived process servers hand out a SNAPSHOT of the env they were
// themselves started with.
//
// The canonical case is tmux. The tmux server is started once — often by
// systemd, or from a TTY login, or by an older graphical session that has
// since ended — and every pane created later inherits THAT env. So a pane
// running under a perfectly healthy Wayland kitty can still see:
//
//     XDG_SESSION_TYPE=tty          (no WAYLAND_DISPLAY at all)
//
// while the same binary run outside tmux sees XDG_SESSION_TYPE=wayland and
// WAYLAND_DISPLAY=wayland-1. tmux's `update-environment` refreshes the
// SESSION environment on attach, but panes that already exist — and panes
// whose server predates the current graphical session — keep the stale
// copy. Nothing the user can configure in tmux fixes an already-running
// pane, which is why this has to be solved here.
//
// Trusting that stale env made read_clipboard_image() skip the wl-paste
// branch entirely and fall through to "clipboard has no image", even
// though `wl-paste --list-types` would have answered `image/png`. The
// failure then got misattributed to the terminal ("the outer terminal
// didn't answer") because the local probe had silently opted out.
//
// So: ask the filesystem, not the environment. A Wayland compositor is
// reachable iff its socket exists in XDG_RUNTIME_DIR, and that fact is
// true regardless of which env snapshot we inherited. When we find a
// socket the env var disagrees with, we pass the corrected value to the
// wl-paste/wl-copy child as an `env VAR=…` prefix on its command — they
// read WAYLAND_DISPLAY themselves and would otherwise fail the same way.
// See DisplayEnv for why this is a per-command prefix and not setenv.

// Directory holding the user's runtime sockets. Falls back to the
// well-known /run/user/<uid> when XDG_RUNTIME_DIR is missing (same stale
// -env scenario; the path is predictable from the uid).
std::string runtime_dir() {
    if (const char* d = std::getenv("XDG_RUNTIME_DIR"); d && *d) return d;
    return "/run/user/" + std::to_string(static_cast<unsigned>(::getuid()));
}

// True when this process is on the far side of an ssh/mosh hop, so the
// clipboard the LOCAL tools can reach is not the one the user copied into.
//
// This matters on macOS specifically. On Linux a remote host usually has no
// display at all, so wl-paste/xclip fail and the code falls through to the
// terminal query by accident. A Mac always has a working pasteboard, so
// pbpaste SUCCEEDS over ssh — it just answers about the wrong machine. The
// read then returns that host's stale text and never asks the terminal,
// which is the one path that can actually reach the laptop's clipboard.
//
// Env sniff, not a syscall: the question is "did someone ssh in", and
// sshd/mosh-server answer it by exporting these. AGENTTY_LOCAL_CLIPBOARD=1
// forces the local tools back on, for the case this sniff gets wrong (a
// remote session that really does own the display the user copied from — a
// VNC/XRDP desktop, say).
bool session_is_remote() {
    if (const char* f = std::getenv("AGENTTY_LOCAL_CLIPBOARD"); f && *f == '1')
        return false;
    for (const char* k : {"SSH_CONNECTION", "SSH_CLIENT", "SSH_TTY"})
        if (const char* v = std::getenv(k); v && *v) return true;
    return false;
}

// Single-quote a value for safe inclusion in a shell command. Every
// embedded quote is closed, escaped, and reopened ('\''). Paths from the
// filesystem can contain anything; this keeps a hostile runtime dir name
// from breaking out of the command we build.
std::string shell_quote(std::string_view s) {
    std::string out;
    out.reserve(s.size() + 2);
    out += '\'';
    for (const char c : s) {
        if (c == '\'') out += "'\\''";
        else           out += c;
    }
    out += '\'';
    return out;
}

// True if `name` resolves to an existing Wayland socket. An absolute
// WAYLAND_DISPLAY is used verbatim (the protocol allows it); otherwise it
// is relative to the runtime dir.
bool wayland_socket_exists(const std::string& dir, std::string_view name) {
    if (name.empty()) return false;
    std::error_code ec;
    std::filesystem::path p =
        name.front() == '/' ? std::filesystem::path{name}
                            : std::filesystem::path{dir} / name;
    // exists() is enough: we only need to know the compositor is there.
    // Checking for S_IFSOCK would be stricter but breaks compositors that
    // expose the display through a bind-mounted path.
    return std::filesystem::exists(p, ec) && !ec;
}

// What we discovered about the display servers on this machine, plus the
// `env VAR=…` prefix that hands the correction to a child process.
//
// The correction is passed PER COMMAND rather than through setenv():
// setenv mutates process-global state, which (a) is not thread-safe
// against a concurrent getenv anywhere else in the program — and POSIX
// gives no way to make it so — and (b) silently changes the environment
// of every unrelated subprocess agentty spawns later, including tools and
// the user's shell. A prefix on the one command that needs it has neither
// problem and is trivially auditable.
struct DisplayEnv {
    bool        wayland = false;   // a compositor socket is reachable
    bool        x11     = false;   // an X server socket is reachable
    std::string prefix;            // "env WAYLAND_DISPLAY=… … " or empty

    // Prepend the correction to a command for popen.
    [[nodiscard]] std::string cmd(std::string_view base) const {
        std::string out{prefix};
        out += base;
        return out;
    }
};

// Resolve the display servers by looking for their SOCKETS rather than
// trusting the inherited environment. See the block comment above.
//
// Not cached: the whole point is that the inherited snapshot can be wrong,
// and a user can start a compositor (or agentty can outlive one) mid
// -session. The cost is a couple of stat() calls per explicit paste, which
// is noise next to the popen we're about to do.
DisplayEnv discover_display() {
    DisplayEnv out;
    const std::string dir = runtime_dir();

    // XDG_RUNTIME_DIR has to reach the child too: wl-paste resolves a
    // relative WAYLAND_DISPLAY against it, so a display name without the
    // directory it lives in just moves the failure one process down.
    const char* env_rt = std::getenv("XDG_RUNTIME_DIR");
    const bool  rt_missing = !env_rt || !*env_rt;

    // ---- Wayland ------------------------------------------------------
    std::string display;

    // Believe the env only if the socket it names actually exists. A
    // WAYLAND_DISPLAY left over from a dead session is worse than none:
    // it makes wl-paste fail with a connect error rather than fall back.
    if (const char* w = std::getenv("WAYLAND_DISPLAY"); w && *w) {
        if (wayland_socket_exists(dir, w)) {
            out.wayland = true;
            display     = w;        // already correct; kept for the prefix
        }
    }

    if (!out.wayland) {
        // Scan for a live compositor socket: wayland-0, wayland-1, …
        // Lowest number wins so a single-compositor box is deterministic.
        std::error_code ec;
        std::vector<std::string> found;
        for (const auto& e : std::filesystem::directory_iterator{dir, ec}) {
            if (ec) break;
            const std::string n = e.path().filename().string();
            if (!n.starts_with("wayland-")) continue;
            if (n.ends_with(".lock")) continue;   // the lock, not the socket
            found.push_back(n);
        }
        if (!found.empty()) {
            std::ranges::sort(found);
            out.wayland = true;
            display     = found.front();
            AGT_LOG(Ui, Info, "clipboard.wayland.repaired",
                    "display={} runtime_dir={} candidates={}",
                    display, dir, found.size());
        }
    }

    // ---- X11 ----------------------------------------------------------
    std::string xdisplay;
    if (const char* d = std::getenv("DISPLAY"); d && *d) {
        out.x11 = true;
    } else {
        std::error_code ec;
        std::vector<std::string> found;
        for (const auto& e :
             std::filesystem::directory_iterator{"/tmp/.X11-unix", ec}) {
            if (ec) break;
            const std::string n = e.path().filename().string();
            if (n.size() > 1 && n.front() == 'X') found.push_back(n.substr(1));
        }
        if (!found.empty()) {
            std::ranges::sort(found);
            out.x11  = true;
            xdisplay = ":" + found.front();
            AGT_LOG(Ui, Info, "clipboard.x11.repaired", "display={}", xdisplay);
        }
    }

    // ---- Build the child-env prefix -----------------------------------
    // Only set what we actually corrected, so a healthy environment runs
    // the bare command and this stays a no-op in the common case.
    std::string vars;
    if (rt_missing) vars += " XDG_RUNTIME_DIR=" + shell_quote(dir);
    if (out.wayland && !display.empty())
        vars += " WAYLAND_DISPLAY=" + shell_quote(display);
    if (!xdisplay.empty())
        vars += " DISPLAY=" + shell_quote(xdisplay);
    if (!vars.empty()) out.prefix = "env" + vars + " ";

    return out;
}

#endif // __linux__

#endif // !_WIN32

std::optional<ClipboardImage> wrap(CaptureResult r) {
    if (r.status != 0 || r.bytes.empty()) return std::nullopt;
    auto* mt = sniff_image_type(r.bytes);
    if (!mt) return std::nullopt;
    ClipboardImage img;
    img.bytes      = std::move(r.bytes);
    img.media_type = mt;
    return img;
}

// Image capture via a user-supplied command (AGENTTY_CLIPBOARD_CMD).
// The override is the airgap escape hatch: the remote agentty has no
// local clipboard, so the user points this at a command that ferries
// the laptop's clipboard image back over the open SSH session (e.g.
// an `ssh` callback to the laptop's wl-paste/pbpaste). Returns:
//   - an image  → override produced decodable bytes
//   - nullopt + clip_handled=false → override unset, fall through to
//     the platform-native path
//   - nullopt + clip_handled=true  → override set but produced no
//     usable image; error_out describes why (don't fall through, the
//     user explicitly chose this path)
std::optional<ClipboardImage>
try_clipboard_cmd_override(std::string* error_out, bool& clip_handled) {
    clip_handled = false;
    const char* cmd = util::env::get_or_null<util::env::Var::ClipboardCmd>();
    if (!cmd) return std::nullopt;
    clip_handled = true;
    auto r = popen_capture(cmd, kCap);
    if (r.bytes.empty()) {
        if (error_out)
            *error_out = "AGENTTY_CLIPBOARD_CMD produced no output "
                         "(clipboard empty, or the command failed)";
        return std::nullopt;
    }
    if (auto img = wrap(std::move(r))) return img;
    if (error_out)
        *error_out = "AGENTTY_CLIPBOARD_CMD output was not a recognised "
                     "image (expected PNG/JPEG/GIF/WEBP bytes on stdout)";
    return std::nullopt;
}

// Kitty clipboard read via the `kitten clipboard` protocol. This is how
// image paste works over a plain SSH session: the `kitten` binary on
// whatever host agentty runs on speaks Kitty's clipboard escape protocol
// to the *local* Kitty terminal over the same TTY/control channel, so
// the bytes cross the SSH link without any clipboard tool on the remote.
//
// We make the kitten write to a temp FILE rather than stdout: the
// stdout/pipe form blocks waiting on the terminal handshake in ways that
// fight a TUI's raw-mode loop, whereas the file form completes and exits.
// Requires Kitty (TERM=xterm-kitty / KITTY_WINDOW_ID) and `kitten` on
// PATH. Returns nullopt (no error_out) when not under Kitty so the
// caller falls through to the native path.
// NOTE: an earlier version shelled out to `kitten clipboard` here to
// fetch the image over Kitty's terminal protocol in a plain SSH session.
// That is fundamentally unsafe from inside a live TUI: the kitten reads
// and writes the controlling /dev/tty that agentty already holds in raw
// mode (bracketed paste, kitty keyboard, mouse, alt-screen). On exit the
// kitten resets terminal modes to its own defaults, corrupting agentty's
// terminal state and crashing the session on the next input. A correct
// implementation must do the clipboard-read escape INSIDE maya's input
// loop (which owns the tty), not via a subprocess. Removed until that
// exists. AGENTTY_CLIPBOARD_CMD remains the supported escape hatch.

} // namespace

std::optional<ClipboardImage> read_clipboard_image(std::string* error_out) {
    auto fail = [&](const char* msg) -> std::optional<ClipboardImage> {
        if (error_out) *error_out = msg;
        return std::nullopt;
    };
    [[maybe_unused]] auto fail_owned =
        [&](std::string msg) -> std::optional<ClipboardImage> {
        if (error_out) *error_out = std::move(msg);
        return std::nullopt;
    };

    // AGENTTY_CLIPBOARD_CMD override runs FIRST, before any platform
    // probe. This is the airgap path: the remote host has no clipboard
    // of its own, so the override ferries the laptop's clipboard image
    // back over the open SSH session. When set, it is authoritative —
    // we don't fall through to wl-paste/xclip/etc. (those would only
    // find the empty remote clipboard and clobber the precise error).
    {
        bool handled = false;
        if (auto img = try_clipboard_cmd_override(error_out, handled))
            return img;
        if (handled) return std::nullopt;  // error_out already set
    }

#if defined(__linux__)
    // Session-type detection. Resolve the display server from its SOCKET
    // rather than from XDG_SESSION_TYPE/WAYLAND_DISPLAY: inside a tmux
    // pane those env vars are a snapshot of whatever started the tmux
    // SERVER, so a healthy Wayland kitty frequently presents as
    // XDG_SESSION_TYPE=tty with no WAYLAND_DISPLAY and we would skip the
    // wl-paste branch that was about to succeed. See discover_display.
    //
    // The result also carries an `env VAR=…` prefix, so the wl-paste
    // child we spawn below connects to the compositor we just found
    // even when our own environment never mentioned it.
    const DisplayEnv disp = discover_display();
    const bool wayland = disp.wayland;
    const bool x11     = disp.x11;

    bool has_wl_paste = tool_in_path("wl-paste");
    bool has_xclip    = tool_in_path("xclip");

    AGT_LOG(Ui, Debug, "clipboard.image.probe",
            "wayland={} x11={} wl_paste={} xclip={} repaired_env={}",
            wayland, x11, has_wl_paste, has_xclip, !disp.prefix.empty());

    if (!has_wl_paste && !has_xclip) {
        // No display server reachable usually means a headless / SSH /
        // airgap host. The image is on the user's laptop, not here —
        // nothing on this machine can read it. Point at the override.
        //
        // Judged by socket probes, not env vars: a stale-env tmux pane on
        // a graphical box is NOT headless, and telling the user to set
        // AGENTTY_CLIPBOARD_CMD there sends them down the wrong path.
        const bool headless = !wayland && !x11;
        AGT_LOG(Ui, Info, "clipboard.image.no_tool",
                "headless={}", headless);
        if (headless) {
            return fail(
                "couldn't read the clipboard over this connection. agentty "
                "asks your terminal for it \xE2\x80\x94 kitty pastes images "
                "and text (OSC 5522); iTerm2/WezTerm/foot/Ghostty paste text "
                "only (OSC 52, enable clipboard read; tmux needs `set -g "
                "set-clipboard on`). Otherwise attach the image by path or "
                "set AGENTTY_CLIPBOARD_CMD");
        }
        return fail(wayland
            ? "no clipboard tool — install wl-clipboard "
              "(`pacman -S wl-clipboard` / `apt install wl-clipboard`)"
            : "no clipboard tool — install xclip "
              "(`pacman -S xclip` / `apt install xclip`)");
    }

    // ---- Backend attempts ---------------------------------------------
    //
    // TRY every installed tool rather than predicting which one "should"
    // work. The socket probe above is a good predictor, but a predictor
    // that is wrong once leaves the user with a dead feature and no way
    // to tell why — exactly the class of bug that made image paste fail
    // inside tmux. Cases a socket probe still can't see: a compositor
    // whose socket lives outside XDG_RUNTIME_DIR, a sandboxed runtime
    // dir we can't enumerate (flatpak/snap/container), an XWayland-only
    // bridge, or a WAYLAND_DISPLAY naming scheme we don't recognise.
    //
    // Trying is cheap and safe: wl-paste/xclip exit non-zero in a few ms
    // when they can't reach a display server, and the whole path only
    // runs on an explicit user paste. So `wayland`/`x11` now only decide
    // the ORDER we try in (ask the native backend first) and the wording
    // of the final error — never whether a backend is attempted at all.
    //
    // Each lambda returns:
    //   - an image           -> done, return it
    //   - nullopt + no error -> backend had nothing, keep going
    //   - nullopt + error    -> backend found an image it couldn't deliver,
    //                           which is worth reporting verbatim
    std::string hard_error;

    auto try_wl_paste = [&]() -> std::optional<ClipboardImage> {
        if (!has_wl_paste) return std::nullopt;
        // Discover what the clipboard is actually offering. wl-paste
        // exits non-zero on an empty clipboard; capture even on
        // failure so we can give a precise diagnostic.
        auto types_r = popen_capture(
            disp.cmd("wl-paste --list-types 2>/dev/null").c_str(), 64 * 1024);
        AGT_LOG(Ui, Debug, "clipboard.image.wl_paste.types",
                "status={} bytes={}", types_r.status, types_r.bytes.size());
        if (types_r.bytes.empty()) return std::nullopt;  // empty, or no compositor

        if (auto* mime = pick_clipboard_image_type(types_r.bytes)) {
            std::string cmd = "wl-paste --type ";
            cmd += mime;
            cmd += " 2>/dev/null";
            if (auto img = wrap(popen_capture(disp.cmd(cmd).c_str(), kCap))) {
                AGT_LOG(Ui, Info, "clipboard.image.ok",
                        "backend=wl-paste mime={} bytes={}",
                        mime, img->bytes.size());
                return img;
            }
            // Listed but failed to capture — usually means the source
            // app died between list and read (KDE's Klipper can race
            // here on Wayland). Real failure: report it if nothing else
            // succeeds, but still let xclip try the X11 bridge.
            hard_error = std::string{"clipboard advertised "} + mime
                       + " but the bytes were unavailable (source app may "
                         "have closed)";
            AGT_LOG(Ui, Warn, "clipboard.image.wl_paste.empty_read",
                    "mime={}", mime);
        } else if (clipboard_has_qt_image_only(types_r.bytes)) {
            hard_error = "clipboard image is in Qt-internal format only "
                         "(application/x-qt-image) — copy from a non-Qt app, "
                         "or take the screenshot via Spectacle's \"Save to "
                         "clipboard\" with the PNG default";
        }
        return std::nullopt;
    };

    auto try_xclip = [&]() -> std::optional<ClipboardImage> {
        if (!has_xclip) return std::nullopt;
        auto targets = popen_capture(
            disp.cmd("xclip -selection clipboard -t TARGETS -o 2>/dev/null").c_str(),
            64 * 1024);
        AGT_LOG(Ui, Debug, "clipboard.image.xclip.targets",
                "status={} bytes={}", targets.status, targets.bytes.size());
        if (auto* mime = pick_clipboard_image_type(targets.bytes)) {
            std::string cmd = "xclip -selection clipboard -t ";
            cmd += mime;
            cmd += " -o 2>/dev/null";
            if (auto img = wrap(popen_capture(disp.cmd(cmd).c_str(), kCap))) {
                AGT_LOG(Ui, Info, "clipboard.image.ok",
                        "backend=xclip mime={} bytes={}",
                        mime, img->bytes.size());
                return img;
            }
        }
        if (hard_error.empty() && clipboard_has_qt_image_only(targets.bytes)) {
            hard_error = "clipboard image is in Qt-internal format only "
                         "(application/x-qt-image) — install wl-clipboard for "
                         "Wayland-native access";
        }
        return std::nullopt;
    };

    // Native backend first, the other as fallback. On Wayland that order
    // matters for correctness, not just speed: Klipper's X11 bridge often
    // has the text but not the image.
    if (x11 && !wayland) {
        if (auto img = try_xclip())    return img;
        if (auto img = try_wl_paste()) return img;
    } else {
        if (auto img = try_wl_paste()) return img;
        if (auto img = try_xclip())    return img;
    }

    if (!hard_error.empty()) return fail_owned(std::move(hard_error));

    AGT_LOG(Ui, Info, "clipboard.image.none",
            "wayland={} wl_paste={} xclip={}", wayland, has_wl_paste, has_xclip);

    // Both tools tried; nothing image-y came back.
    if (wayland && !has_wl_paste) {
        return fail("clipboard has no image — Wayland session needs "
                    "wl-clipboard for native access (xclip works only "
                    "via Klipper's X11 bridge, which doesn't always "
                    "carry images)");
    }
    return fail("clipboard has no image");

#elif defined(__APPLE__)
    // SKIPPED over ssh — and this is the whole macOS image-paste bug.
    //
    // On Linux a remote host usually has no display, so wl-paste/xclip fail
    // and the read falls through to the terminal query BY ACCIDENT. A Mac
    // always has a working pasteboard, so pngpaste/osascript SUCCEED over
    // ssh; they just answer about the wrong machine. "clipboard has no
    // image" from the far end then hid the fact that the user's laptop had
    // a screenshot on it, and the terminal — the only path that can reach
    // that clipboard — was never asked.
    if (session_is_remote())
        return fail("ssh session \xe2\x80\x94 the pasteboard on this host is "
                    "not the one you copied into");
    if (tool_in_path("pngpaste")) {
        if (auto img = wrap(popen_capture("pngpaste - 2>/dev/null", kCap)))
            return img;
        return fail("clipboard has no image");
    }
    // osascript fallback — slower (~150-300 ms) but always available.
    auto r = popen_capture(
        "set -e; "
        "f=$(mktemp -t agentty-clip).png; "
        "trap 'rm -f \"$f\"' EXIT; "
        "osascript -e 'set png to (the clipboard as «class PNGf»)' "
        "          -e 'set fh to open for access POSIX file \"'\"$f\"'\" "
        "                 with write permission' "
        "          -e 'write png to fh' "
        "          -e 'close access fh' >/dev/null 2>&1 && cat \"$f\"",
        kCap);
    if (auto img = wrap(std::move(r))) return img;
    return fail("clipboard has no image (install pngpaste for a faster path: "
                "`brew install pngpaste`)");

#elif defined(_WIN32)
    // Direct Win32 Clipboard API. The previous PowerShell shell-out
    // had two failure modes that made Ctrl+V silently no-op:
    //
    //   1. Quoting: _popen launches `cmd /C <our string>`. The full
    //      script contained `(`, `)`, `;`, `$`, `[`, `]` — characters
    //      cmd treats as either special or shell-meta depending on the
    //      parent process's environment. Under MSYS2 / Git Bash, the
    //      command was visibly mangled before reaching powershell.
    //
    //   2. PowerShell startup tax: even when quoting worked, the user
    //      paid 200–500 ms of PS spin-up per paste. On a healthy
    //      clipboard the diagnostic was `wrap()` returning nullopt
    //      because of a non-zero status, which surfaced as the generic
    //      "no image on clipboard" toast — hiding the real cause.
    //
    // Going through user32/gdiplus eliminates the shell entirely. ~ms-
    // scale paste, precise errors, no external dependency.

    if (!::OpenClipboard(nullptr))
        return fail("could not open Windows clipboard (another process may hold it)");
    struct ClipGuard {
        ~ClipGuard() { ::CloseClipboard(); }
    } clip_guard;

    // (a) Fast path: clipboard already carries native PNG bytes. Browsers,
    //     Discord, Slack, ShareX, Greenshot, etc. register the "PNG"
    //     format alongside CF_DIB so we can copy without re-encoding.
    if (UINT png_fmt = ::RegisterClipboardFormatW(L"PNG");
        png_fmt && ::IsClipboardFormatAvailable(png_fmt))
    {
        if (HANDLE h = ::GetClipboardData(png_fmt); h) {
            auto* data = static_cast<const char*>(::GlobalLock(h));
            const SIZE_T size = data ? ::GlobalSize(h) : 0;
            if (data && size > 0) {
                std::string bytes(data,
                                  std::min<std::size_t>(size, kCap));
                ::GlobalUnlock(h);
                if (auto img = wrap(CaptureResult{std::move(bytes), 0}))
                    return img;
            } else if (data) {
                ::GlobalUnlock(h);
            }
        }
    }

    // GDI+ scope: shared across (b) DIB path and (c) CF_BITMAP fallback.
    // Per-call startup; a process-wide token would shave ~1 ms but adds
    // lifecycle the rest of the app doesn't need.
    Gdiplus::GdiplusStartupInput gdi_in;
    ULONG_PTR gdi_token = 0;
    if (Gdiplus::GdiplusStartup(&gdi_token, &gdi_in, nullptr) != Gdiplus::Ok)
        return fail("GDI+ startup failed");
    struct GdiGuard {
        ULONG_PTR tok;
        ~GdiGuard() { Gdiplus::GdiplusShutdown(tok); }
    } gdi_guard{gdi_token};

    // Lazy-found PNG encoder CLSID. There's no GetEncoderByMime helper —
    // walk the codec list once and remember the result for both paths.
    auto find_png_encoder = [&](CLSID& out) -> bool {
        UINT num = 0, sz = 0;
        Gdiplus::GetImageEncodersSize(&num, &sz);
        if (num == 0 || sz == 0) return false;
        std::vector<BYTE> buf(sz);
        auto* codecs = reinterpret_cast<Gdiplus::ImageCodecInfo*>(buf.data());
        Gdiplus::GetImageEncoders(num, sz, codecs);
        for (UINT i = 0; i < num; ++i) {
            if (std::wcscmp(codecs[i].MimeType, L"image/png") == 0) {
                out = codecs[i].Clsid;
                return true;
            }
        }
        return false;
    };

    // Encode a GDI+ Bitmap to PNG bytes via an in-memory IStream.
    auto encode_to_png = [&](Gdiplus::Bitmap& bitmap,
                             std::string& out_bytes,
                             std::string& out_err) -> bool
    {
        CLSID png_clsid{};
        if (!find_png_encoder(png_clsid)) {
            out_err = "GDI+ PNG encoder not registered on this system";
            return false;
        }
        IStream* out_stream = ::SHCreateMemStream(nullptr, 0);
        if (!out_stream) { out_err = "SHCreateMemStream(out) failed"; return false; }
        struct StreamRelease { IStream* s; ~StreamRelease(){ if(s) s->Release(); } } sg{out_stream};

        if (bitmap.Save(out_stream, &png_clsid, nullptr) != Gdiplus::Ok) {
            out_err = "GDI+ PNG encode failed";
            return false;
        }
        STATSTG stat{};
        if (out_stream->Stat(&stat, STATFLAG_NONAME) != S_OK) {
            out_err = "PNG stream Stat failed";
            return false;
        }
        const auto png_size = static_cast<std::size_t>(stat.cbSize.QuadPart);
        if (png_size == 0 || png_size > kCap) {
            out_err = "PNG output size out of range";
            return false;
        }
        LARGE_INTEGER zero{};
        out_stream->Seek(zero, STREAM_SEEK_SET, nullptr);
        out_bytes.assign(png_size, '\0');
        ULONG bytes_read = 0;
        if (out_stream->Read(out_bytes.data(),
                             static_cast<ULONG>(png_size),
                             &bytes_read) != S_OK) {
            out_err = "PNG stream Read failed";
            return false;
        }
        out_bytes.resize(bytes_read);
        return true;
    };

    // (b) DIB fallback: the standard "Win+Shift+S / Snipping Tool /
    //     PrintScreen" path. Re-attach a BITMAPFILEHEADER so the bytes
    //     are a complete BMP file, then re-encode through GDI+ to PNG
    //     (Anthropic's image API doesn't accept BMP).

    if (const UINT dib_fmt =
              ::IsClipboardFormatAvailable(CF_DIBV5) ? CF_DIBV5
            : ::IsClipboardFormatAvailable(CF_DIB)   ? CF_DIB
            : 0u;
        dib_fmt != 0)
    {
        HANDLE dh = ::GetClipboardData(dib_fmt);
        if (dh) {
            auto* dib = static_cast<const BYTE*>(::GlobalLock(dh));
            const SIZE_T dib_size = dib ? ::GlobalSize(dh) : 0;
            struct UnlockGuard {
                HANDLE h;
                ~UnlockGuard() { if (h) ::GlobalUnlock(h); }
            } unlock_guard{dh};
            if (dib && dib_size >= sizeof(BITMAPINFOHEADER)) {
                // Compute where the pixel array starts inside the DIB
                // block. The hairy bit: bitfield masks only appear as a
                // separate trailing block when the header is the V3
                // BITMAPINFOHEADER (size 40). V4 (108) and V5 (124)
                // store the masks INLINE, so adding 12 bytes shifts
                // every pixel of a modern Win+Shift+S screenshot —
                // Snipping Tool writes CF_DIBV5 with
                // biCompression=BI_BITFIELDS, which is exactly the case
                // the previous code mis-handled.
                const auto* hdr = reinterpret_cast<const BITMAPINFOHEADER*>(dib);
                DWORD masks_bytes = 0;
                if (hdr->biSize == sizeof(BITMAPINFOHEADER)) {
                    if (hdr->biCompression == BI_BITFIELDS)
                        masks_bytes = 3 * sizeof(DWORD);
                    else if (hdr->biCompression == 6 /* BI_ALPHABITFIELDS */)
                        masks_bytes = 4 * sizeof(DWORD);
                }
                DWORD palette_bytes = 0;
                if (hdr->biBitCount <= 8) {
                    const DWORD n = hdr->biClrUsed ? hdr->biClrUsed
                                                   : (1u << hdr->biBitCount);
                    palette_bytes = n * sizeof(RGBQUAD);
                }
                const DWORD pixels_offset =
                    static_cast<DWORD>(sizeof(BITMAPFILEHEADER))
                  + hdr->biSize + masks_bytes + palette_bytes;

                std::string bmp;
                bmp.resize(sizeof(BITMAPFILEHEADER) + dib_size);
                BITMAPFILEHEADER bfh{};
                bfh.bfType    = 0x4D42; // 'BM'
                bfh.bfSize    = static_cast<DWORD>(bmp.size());
                bfh.bfOffBits = pixels_offset;
                std::memcpy(bmp.data(), &bfh, sizeof(bfh));
                std::memcpy(bmp.data() + sizeof(bfh), dib, dib_size);

                IStream* bmp_stream = ::SHCreateMemStream(
                    reinterpret_cast<const BYTE*>(bmp.data()),
                    static_cast<UINT>(bmp.size()));
                if (bmp_stream) {
                    struct StreamRelease { IStream* s; ~StreamRelease(){ if(s) s->Release(); } } bg{bmp_stream};
                    std::unique_ptr<Gdiplus::Bitmap> bitmap{
                        Gdiplus::Bitmap::FromStream(bmp_stream)};
                    if (bitmap && bitmap->GetLastStatus() == Gdiplus::Ok) {
                        std::string png_bytes, enc_err;
                        if (encode_to_png(*bitmap, png_bytes, enc_err)) {
                            if (auto img = wrap(CaptureResult{std::move(png_bytes), 0}))
                                return img;
                        }
                    }
                }
            }
        }
        // Fall through to CF_BITMAP — some sources publish a malformed
        // DIB but a valid HBITMAP for the same pixels.
    }

    // (c) CF_BITMAP fallback: a few apps only register an HBITMAP, even
    //     though Windows is supposed to auto-synthesize CF_DIB from it.
    //     FromHBITMAP does the offset math for us.
    if (::IsClipboardFormatAvailable(CF_BITMAP)) {
        if (auto hbm = static_cast<HBITMAP>(::GetClipboardData(CF_BITMAP)); hbm) {
            std::unique_ptr<Gdiplus::Bitmap> bitmap{
                Gdiplus::Bitmap::FromHBITMAP(hbm, nullptr)};
            if (bitmap && bitmap->GetLastStatus() == Gdiplus::Ok) {
                std::string png_bytes, enc_err;
                if (encode_to_png(*bitmap, png_bytes, enc_err)) {
                    if (auto img = wrap(CaptureResult{std::move(png_bytes), 0}))
                        return img;
                }
                return fail_owned("CF_BITMAP path failed: " +
                                  (enc_err.empty() ? std::string{"PNG sniff failed"} : enc_err));
            }
        }
    }

    return fail("clipboard has no image (no PNG/DIBV5/DIB/CF_BITMAP format present)");
#else
    return fail("clipboard image read not implemented on this platform");
#endif
}

// ===========================================================================
// read_clipboard_text — plain text variant for the smart-paste path
// ===========================================================================

std::optional<std::string> read_clipboard_text(std::string* error_out) {
    auto fail = [&](const char* msg) -> std::optional<std::string> {
        if (error_out) *error_out = msg;
        return std::nullopt;
    };

#if defined(__linux__)
    // Socket-probed, not env-sniffed — same stale-tmux-env reason as
    // read_clipboard_image(). See discover_display().
    const DisplayEnv disp = discover_display();
    const bool wayland = disp.wayland;
    const bool x11     = disp.x11;

    // TRY both backends; detection only picks the order. Same rationale
    // as read_clipboard_image() — a wrong prediction must not be able to
    // disable a backend that would have worked.
    auto wl = [&]() -> std::optional<std::string> {
        if (!tool_in_path("wl-paste")) return std::nullopt;
        auto r = popen_capture(
            disp.cmd("wl-paste --no-newline 2>/dev/null").c_str(), kCap);
        AGT_LOG(Ui, Debug, "clipboard.text.wl_paste",
                "status={} bytes={}", r.status, r.bytes.size());
        if (r.status == 0 && !r.bytes.empty()) return std::move(r.bytes);
        return std::nullopt;
    };
    auto xc = [&]() -> std::optional<std::string> {
        if (!tool_in_path("xclip")) return std::nullopt;
        auto r = popen_capture(
            disp.cmd("xclip -selection clipboard -o 2>/dev/null").c_str(), kCap);
        AGT_LOG(Ui, Debug, "clipboard.text.xclip",
                "status={} bytes={}", r.status, r.bytes.size());
        if (r.status == 0 && !r.bytes.empty()) return std::move(r.bytes);
        return std::nullopt;
    };

    if (x11 && !wayland) {
        if (auto t = xc()) return t;
        if (auto t = wl()) return t;
    } else {
        if (auto t = wl()) return t;
        if (auto t = xc()) return t;
    }

    AGT_LOG(Ui, Debug, "clipboard.text.none", "wayland={} x11={}", wayland, x11);
    return fail("clipboard has no text");

#elif defined(__APPLE__)
    // SKIPPED over ssh, and this is the half that actually broke image paste.
    //
    // smart_paste_from_clipboard tries image, then TEXT, then the terminal.
    // pbpaste succeeds over ssh against the wrong machine, so any stale text
    // on the remote host satisfied the text step and the function returned
    // before ever asking the terminal — the user pressed Ctrl+V on a fresh
    // screenshot and got whatever they last copied on the server.
    //
    // Refusing here is what lets the fallback chain reach OSC 5522, which
    // carries the real image back from kitty over the pty.
    if (session_is_remote())
        return fail("ssh session \xe2\x80\x94 the pasteboard on this host is "
                    "not the one you copied into");
    auto r = popen_capture("pbpaste 2>/dev/null", kCap);
    if (r.status == 0 && !r.bytes.empty()) return std::move(r.bytes);
    return fail("clipboard has no text");

#elif defined(_WIN32)
    if (!::OpenClipboard(nullptr))
        return fail("could not open Windows clipboard");
    struct ClipGuard {
        ~ClipGuard() { ::CloseClipboard(); }
    } clip_guard;

    // CF_UNICODETEXT first — modern apps write Unicode. CF_TEXT is the
    // legacy fallback (system-codepage), but Windows auto-synthesises
    // it from CF_UNICODETEXT and vice-versa, so checking UNICODETEXT
    // alone covers nearly everything.
    if (::IsClipboardFormatAvailable(CF_UNICODETEXT)) {
        if (HANDLE h = ::GetClipboardData(CF_UNICODETEXT); h) {
            auto* wide = static_cast<const wchar_t*>(::GlobalLock(h));
            struct UnlockGuard {
                HANDLE h;
                ~UnlockGuard() { if (h) ::GlobalUnlock(h); }
            } unlock_guard{h};
            if (!wide) return fail("GlobalLock(CF_UNICODETEXT) failed");

            // wcslen — clipboard text is NUL-terminated by the
            // Windows clipboard contract.
            const int wide_len = static_cast<int>(std::wcslen(wide));
            if (wide_len == 0) return fail("clipboard text is empty");

            const int utf8_len = ::WideCharToMultiByte(
                CP_UTF8, 0, wide, wide_len, nullptr, 0, nullptr, nullptr);
            if (utf8_len <= 0) return fail("UTF-8 conversion failed");
            std::string out(static_cast<std::size_t>(utf8_len), '\0');
            ::WideCharToMultiByte(CP_UTF8, 0, wide, wide_len,
                                  out.data(), utf8_len, nullptr, nullptr);
            return out;
        }
    }
    return fail("clipboard has no text");

#else
    return fail("clipboard text read not implemented on this platform");
#endif
}

// ===========================================================================
// write_clipboard_text — native clipboard WRITE (OSC-52-independent)
// ===========================================================================
//
// Pipes the payload into a platform clipboard tool's stdin. Uses popen in
// write mode; the payload is delivered as raw bytes (no shell quoting of
// the content — only the fixed tool command is a string literal, so there
// is no injection surface from `text`).

namespace {

// popen a command for WRITING and feed it `data` on stdin. Returns true
// iff the child exited 0. status==-1 (popen failed) or a non-zero exit
// both report false. Windows maps popen()→_popen via the macros above.
bool popen_write(const char* cmd, std::string_view data) {
#if defined(_WIN32)
    FILE* fp = ::_popen(cmd, "wb");
#else
    FILE* fp = ::popen(cmd, "w");
#endif
    if (!fp) return false;
    bool wrote_ok = true;
    if (!data.empty()) {
        const std::size_t n =
            std::fwrite(data.data(), 1, data.size(), fp);
        wrote_ok = (n == data.size());
    }
#if defined(_WIN32)
    const int status = ::_pclose(fp);
#else
    const int status = ::pclose(fp);
#endif
    // pclose returns the child wait-status; 0 means clean exit 0.
    return wrote_ok && status == 0;
}

} // namespace

bool write_clipboard_text(std::string_view text, std::string* error_out) {
    auto fail = [&](const char* msg) -> bool {
        if (error_out) *error_out = msg;
        return false;
    };

#if defined(__APPLE__)
    if (popen_write("pbcopy", text)) return true;
    return fail("pbcopy failed (is it on PATH?)");

#elif defined(__linux__)
    // Socket-probed, not env-sniffed — same stale-tmux-env reason as
    // read_clipboard_image(). Without this a copy from a tmux pane
    // silently failed to reach the system clipboard. See
    // discover_display().
    const DisplayEnv disp = discover_display();
    const bool wayland = disp.wayland;
    const bool x11     = disp.x11;

    // Try every installed tool; detection only orders them. wl-copy is
    // asked first on Wayland because xclip/xsel reach a Wayland
    // clipboard only through XWayland, which some compositors don't
    // bridge for writes.
    auto attempt = [&](const char* tool, const char* base) {
        if (!tool_in_path(tool)) return false;
        const bool ok = popen_write(disp.cmd(base).c_str(), text);
        AGT_LOGL(Ui, ok ? ::agentty::logx::Level::Info
                        : ::agentty::logx::Level::Debug,
                 "clipboard.write", "tool={} ok={}", tool, ok);
        return ok;
    };

    if (wayland) {
        if (attempt("wl-copy", "wl-copy 2>/dev/null")) return true;
    }
    if (attempt("xclip", "xclip -selection clipboard 2>/dev/null")) return true;
    if (attempt("xsel", "xsel --clipboard --input 2>/dev/null")) return true;
    // Last resort on a box where the socket probe said "no compositor"
    // but wl-copy is installed — cheap, and covers a compositor whose
    // socket we couldn't see (sandboxed runtime dir, odd socket path).
    if (!wayland) {
        if (attempt("wl-copy", "wl-copy 2>/dev/null")) return true;
    }

    AGT_LOG(Ui, Warn, "clipboard.write.failed",
            "wayland={} x11={}", wayland, x11);
    return fail("no clipboard tool — install wl-clipboard or xclip");

#elif defined(_WIN32)
    // clip.exe reads stdin and sets CF_UNICODETEXT/CF_TEXT. It expects
    // the active codepage; agentty text is UTF-8, which clip.exe on
    // modern Windows accepts for the common ASCII/Latin range. Good
    // enough for code blocks; OSC 52 covers the rest via the batch call.
    if (popen_write("clip.exe", text)) return true;
    return fail("clip.exe failed");

#else
    return fail("clipboard text write not implemented on this platform");
#endif
}

} // namespace agentty
