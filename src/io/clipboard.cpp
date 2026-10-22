// The system clipboard. Reads go through richpaste; writes use the
// platform tool. See include/agentty/io/clipboard.hpp.

#include "agentty/io/clipboard.hpp"
#include "agentty/util/logx.hpp"
#include "agentty/util/snapshot.hpp"   // AtomicSnapshot

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#if !defined(_WIN32)
#  include <unistd.h>   // access/X_OK (tool_in_path), getuid
#endif

#include <maya/runtime.hpp>
#include "agentty/tool/util/exec.hpp"   // run_child

#include <rp/agent.hpp>
#include <rp/armor.hpp>
#include <rp/cli.hpp>
#include <rp/clipboard.hpp>  // rp::write_clipboard_text
#include <rp/store.hpp>      // rp::store

namespace agentty {

// ── Reading ────────────────────────────────────────────────────────────────

namespace {

ClipboardItem to_item(const rp::Item& it) {
    ClipboardItem out;
    out.mime = std::string{rp::mime_of(it)};
    out.name = std::string{rp::name_of(it)};
    auto data = rp::payload(it);
    out.bytes.assign(reinterpret_cast<const char*>(data.data()), data.size());
    switch (rp::kind_of(it)) {
        case rp::Kind::text:  out.kind = ClipboardItem::Kind::Text;  break;
        case rp::Kind::image: out.kind = ClipboardItem::Kind::Image; break;
        case rp::Kind::file:  out.kind = ClipboardItem::Kind::File;  break;
    }
    return out;
}

// The installed settings: swapped whole on a Settings change, read per paste.
util::AtomicSnapshot<paste_cfg::Config>& cfg_slot() {
    static util::AtomicSnapshot<paste_cfg::Config> s;
    return s;
}

paste_cfg::Config current_cfg() {
    const auto s = cfg_slot().load();
    return s ? *s : paste_cfg::Config{};
}

rp::TerminalMode terminal_mode(std::string_view s) {
    if (s == "osc5522") return rp::TerminalMode::osc5522;
    if (s == "osc52")   return rp::TerminalMode::osc52;
    if (s == "off")     return rp::TerminalMode::off;
    return rp::TerminalMode::automatic;
}

} // namespace

void clipboard_configure(const paste_cfg::Config& c) {
    rp::Config r;
    r.forwards       = c.forwards;
    r.display        = c.display;
    r.server_display = c.server_display;
    r.terminal       = terminal_mode(c.terminal);
    r.decode_text    = c.decode_text;
    r.max_bytes      = static_cast<std::uint64_t>(
                           std::clamp(c.max_mb, paste_cfg::kMaxMbMin, paste_cfg::kMaxMbMax)) << 20;
    r.socket         = c.socket;
    r.save_dir       = c.save_dir;
    // Both are swapped whole; a paste already running keeps what it read.
    cfg_slot().store(c);
    rp::set_config(std::move(r));
}

paste_cfg::Config clipboard_config() { return current_cfg(); }

std::filesystem::path clipboard_save_dir() {
    auto c = current_cfg();
    if (!c.save_dir.empty()) return c.save_dir;
    return rp::state_dir() / "pasted";
}

ClipboardContents read_clipboard(bool allow_remote) {
    ClipboardContents out;
    rp::PasteOptions opt;
    // No terminal session: the tty belongs to maya, and the composer asks
    // the terminal itself (OSC 5522 / 52) when this comes back empty.
    auto clip = allow_remote ? rp::paste(opt) : rp::read_clipboard(opt.max_bytes);
    if (!clip) {
        out.error = clip.error().what;
        AGT_LOG(Ui, Info, "clipboard.read.none", "why={}", out.error);
        return out;
    }
    out.source = clip->source;
    for (auto& it : clip->items) out.items.push_back(to_item(it));
    AGT_LOG(Ui, Info, "clipboard.read", "source={} items={}", out.source, out.items.size());
    return out;
}

bool has_armored(std::string_view text) noexcept {
    return text.find(rp::armor::magic) != std::string_view::npos;
}

bool looks_like_clipboard_reply(std::string_view text) noexcept {
    return text.find("5522;type=read:status=") != std::string_view::npos;
}

std::string salvage_clipboard_reply(std::string_view text) {
    // OSC 5522 read reply whose ESC bytes were lost (some multiplexer
    // setups strip them), so it reached the input as plain text:
    //   ]5522;type=read:status=DATA:mime=<b64 mime>;<b64 chunk>\ ...
    // Keep the chunks of the first mime offered, decode them, done.
    std::string mime, b64;
    constexpr std::string_view kData = "5522;type=read:status=DATA:";
    for (std::size_t at = text.find(kData); at != std::string_view::npos;
         at = text.find(kData, at + kData.size())) {
        auto meta = text.substr(at + kData.size());
        const auto semi = meta.find(';');
        if (semi == std::string_view::npos) break;
        std::string m;
        for (std::string_view kv = meta.substr(0, semi); !kv.empty();) {
            const auto colon = kv.find(':');
            const auto one = kv.substr(0, colon);
            if (one.starts_with("mime=")) m = std::string{one.substr(5)};
            kv = colon == std::string_view::npos ? std::string_view{} : kv.substr(colon + 1);
        }
        if (mime.empty()) mime = m;
        if (m != mime) continue;
        auto data = meta.substr(semi + 1);
        std::size_t n = 0;
        while (n < data.size() && (std::isalnum(static_cast<unsigned char>(data[n]))
                                   || data[n] == '+' || data[n] == '/' || data[n] == '=')) ++n;
        b64 += data.substr(0, n);
    }
    if (b64.empty()) return {};
    // Chunks may end on padding mid stream; decode group by group.
    std::string out;
    for (std::size_t i = 0; i + 4 <= b64.size(); i += 4) {
        auto d = rp::base64_decode(std::string_view{b64}.substr(i, 4));
        if (!d) return {};
        out.append(reinterpret_cast<const char*>(d->data()), d->size());
    }
    return out;
}

bool armored_complete_enough(std::string_view text) noexcept {
    // richpaste:1:<mime>:<name>:<size>:<sha>:<data>
    auto at = text.find(rp::armor::magic);
    if (at == std::string_view::npos) return false;
    std::string_view rest = text.substr(at + rp::armor::magic.size());
    for (int skip = 0; skip < 2; ++skip) {
        auto c = rest.find(':');
        if (c == std::string_view::npos) return false;
        rest.remove_prefix(c + 1);
    }
    std::uint64_t size = 0;
    std::size_t i = 0;
    for (; i < rest.size() && rest[i] >= '0' && rest[i] <= '9' && i < 19; ++i)
        size = size * 10 + static_cast<std::uint64_t>(rest[i] - '0');
    if (i == 0 || i == rest.size() || rest[i] != ':') return false;
    return rest.size() >= i + size / 3 * 4;
}

std::vector<ClipboardItem> decode_armored(std::string_view text, std::string* error) {
    std::vector<ClipboardItem> out;
    if (!has_armored(text)) return out;
    auto sp = rp::armor::split(text);
    if (error && !sp.errors.empty()) *error = sp.errors.front();
    bool any = false;
    for (auto& part : sp.parts) {
        if (auto* s = std::get_if<std::string>(&part)) {
            if (s->find_first_not_of(" \t\r\n") == std::string::npos) continue;
            out.push_back({ClipboardItem::Kind::Text, "text/plain;charset=utf-8", {}, *s});
        } else {
            out.push_back(to_item(std::get<rp::Item>(part)));
            any = true;
        }
    }
    if (!any) out.clear();   // every block was bad: the caller pastes the text as is
    return out;
}

void settle_clipboard_items(std::vector<ClipboardItem>& items) {
    const std::size_t inline_bytes = static_cast<std::size_t>(std::max(0, current_cfg().inline_kb)) * 1024;
    for (auto& it : items) {
        if (it.kind != ClipboardItem::Kind::File) continue;
        if (it.mime.starts_with("image/")) {
            it.kind = ClipboardItem::Kind::Image;
            continue;
        }
        // A small UTF-8 file with no NULs goes in as its text.
        const bool texty = inline_bytes > 0 && it.bytes.size() <= inline_bytes
                        && it.bytes.find('\0') == std::string::npos
                        && rp::valid_utf8(it.bytes);
        if (texty) {
            it.kind  = ClipboardItem::Kind::Text;
            it.bytes = "// file: " + it.name + "\n" + it.bytes;
            continue;
        }
        // Anything else is saved, and the path goes in the box.
        rp::Item ri = rp::File{rp::FileName::sanitize(it.name),
                               rp::Mime::parse(it.mime).value_or(rp::mime_for_name(it.name)),
                               rp::to_bytes(it.bytes)};
        auto stored = rp::store(std::span{&ri, 1}, clipboard_save_dir());
        it.kind = ClipboardItem::Kind::Text;
        it.mime = "text/plain;charset=utf-8";
        if (stored && !stored->parts.empty()) {
            if (auto* saved = std::get_if<rp::Saved>(&stored->parts.front())) {
                it.bytes      = saved->path.string();
                it.saved_path = true;
            }
        } else {
            it.bytes = it.name;   // couldn't save: at least say what it was
            AGT_LOG(Ui, Warn, "clipboard.save_failed", "name={} why={}", it.name,
                    stored ? std::string{"empty"} : stored.error().what);
        }
    }
}

// ── agentty paste ──────────────────────────────────────────────────────────

int clipboard_cli(std::span<const std::string> args) { return rp::cli::run(args); }

ClipboardForward clipboard_ssh_forward(std::string* error) {
    auto f = rp::remote_forward();
    if (!f) {
        if (error) *error = f.error().what;
        return {};
    }
    return {std::move(f->first), std::move(f->second)};
}

// ── Writing ────────────────────────────────────────────────────────────────

namespace {

#if !defined(_WIN32)

// Run a clipboard tool and feed it `data` on stdin. True iff it exited 0.
bool popen_write(const char* cmd, std::string_view data) {
    tools::util::ChildRun run;
    run.argv = {"/bin/sh", "-c", cmd};
    run.stdin_data = std::string{data};
    run.idle       = std::chrono::seconds{10};
    run.wall       = std::chrono::seconds{30};
    run.max_output_bytes = 4096;
    auto c = tools::util::run_child(run);
    return c.started && c.exited && c.exit_code == 0;
}

#if defined(__linux__)

// Is `name` an executable on PATH? No shell, and no names with a slash.
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
        rest = (colon == std::string_view::npos) ? std::string_view{} : rest.substr(colon + 1);
        std::string cand{dir.empty() ? std::string_view{"."} : dir};
        if (cand.back() != '/') cand += '/';
        cand += n;
        if (::access(cand.c_str(), X_OK) == 0) return true;
    }
    return false;
}

std::string shell_quote(std::string_view s) {
    std::string out = "'";
    for (const char c : s) {
        if (c == '\'') out += "'\\''";
        else           out += c;
    }
    out += '\'';
    return out;
}

// Inside tmux, WAYLAND_DISPLAY/DISPLAY are whatever the tmux server started
// with, often nothing. So find the display by its socket and pass it to the
// one child that needs it (setenv would race other threads' getenv).
struct DisplayEnv {
    bool        wayland = false;
    bool        x11     = false;
    std::string prefix;   // "env VAR=... " or empty
    [[nodiscard]] std::string cmd(std::string_view base) const {
        return prefix + std::string{base};
    }
};

DisplayEnv discover_display() {
    namespace fs = std::filesystem;
    DisplayEnv out;
    const char* env_rt = std::getenv("XDG_RUNTIME_DIR");
    const bool rt_missing = !env_rt || !*env_rt;
    const std::string dir = rt_missing
        ? "/run/user/" + std::to_string(static_cast<unsigned>(::getuid())) : env_rt;
    std::error_code ec;

    std::string display;
    if (const char* w = std::getenv("WAYLAND_DISPLAY"); w && *w) {
        fs::path p = *w == '/' ? fs::path{w} : fs::path{dir} / w;
        if (fs::exists(p, ec)) { out.wayland = true; display = w; }
    }
    if (!out.wayland) {
        std::vector<std::string> found;
        for (const auto& e : fs::directory_iterator{dir, ec}) {
            const std::string n = e.path().filename().string();
            if (n.starts_with("wayland-") && !n.ends_with(".lock")) found.push_back(n);
        }
        if (!found.empty()) {
            std::ranges::sort(found);
            out.wayland = true;
            display = found.front();
        }
    }

    std::string xdisplay;
    if (const char* d = std::getenv("DISPLAY"); d && *d) {
        out.x11 = true;
    } else {
        std::vector<std::string> found;
        for (const auto& e : fs::directory_iterator{"/tmp/.X11-unix", ec}) {
            const std::string n = e.path().filename().string();
            if (n.size() > 1 && n.front() == 'X') found.push_back(n.substr(1));
        }
        if (!found.empty()) {
            std::ranges::sort(found);
            out.x11 = true;
            xdisplay = ":" + found.front();
        }
    }

    std::string vars;
    if (rt_missing) vars += " XDG_RUNTIME_DIR=" + shell_quote(dir);
    if (out.wayland && !display.empty()) vars += " WAYLAND_DISPLAY=" + shell_quote(display);
    if (!xdisplay.empty()) vars += " DISPLAY=" + shell_quote(xdisplay);
    if (!vars.empty()) out.prefix = "env" + vars + " ";
    return out;
}

#endif // __linux__

#endif // !_WIN32

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
    const DisplayEnv disp = discover_display();
    // Try every installed tool; detection only orders them. wl-copy first on
    // Wayland: xclip reaches it only through XWayland.
    auto attempt = [&](const char* tool, const char* base) {
        if (!tool_in_path(tool)) return false;
        const bool ok = popen_write(disp.cmd(base).c_str(), text);
        AGT_LOGL(Ui, ok ? ::agentty::logx::Level::Info : ::agentty::logx::Level::Debug,
                 "clipboard.write", "tool={} ok={}", tool, ok);
        return ok;
    };
    if (disp.wayland && attempt("wl-copy", "wl-copy 2>/dev/null")) return true;
    if (attempt("xclip", "xclip -selection clipboard 2>/dev/null")) return true;
    if (attempt("xsel", "xsel --clipboard --input 2>/dev/null")) return true;
    if (!disp.wayland && attempt("wl-copy", "wl-copy 2>/dev/null")) return true;
    AGT_LOG(Ui, Warn, "clipboard.write.failed", "wayland={} x11={}", disp.wayland, disp.x11);
    return fail("no clipboard tool — install wl-clipboard or xclip");

#elif defined(_WIN32)
    // Win32 CF_UNICODETEXT directly: clip.exe mangles non-ASCII text.
    auto r = rp::write_clipboard_text(text);
    if (r) return true;
    return fail(r.error().what.c_str());

#else
    return fail("clipboard text write not implemented on this platform");
#endif
}

} // namespace agentty
