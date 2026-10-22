// paste_cli.cpp — `agentty paste …`, drawn with maya.
//
// richpaste does the work and hands back data (rp/report.hpp); this file only
// decides how it looks. Commands whose output is meant for a pipe (`paste`,
// `page`, `encode`, `decode`, the snippets) and the background ones (`agent`,
// `ssh-hook`) still go straight to richpaste's plain command line.

#include "agentty/runtime/paste_cli.hpp"

#include "agentty/io/clipboard.hpp"
#include "agentty/io/persistence.hpp"
#include "agentty/runtime/settings_registry.hpp"
#include "agentty/runtime/view/palette.hpp"

#include <maya/dsl.hpp>
#include <maya/element/builder.hpp>
#include <maya/print.hpp>
#include <maya/style/theme.hpp>

#include <rp/armor.hpp>
#include <rp/report.hpp>
#include <rp/sha256.hpp>

#include <algorithm>
#include <cstdio>
#include <format>
#include <string>
#include <unistd.h>
#include <vector>

namespace agentty::paste_cli {

namespace reg = settings::registry;
using maya::Element;
using namespace maya::dsl;

namespace {

// ── drawing ──────────────────────────────────────────────────────────────

bool painted(FILE* f) {
    return ::isatty(fileno(f)) == 1 && maya::theme::detect_tier(true) != maya::theme::ColorTier::Mono;
}

int width_of(FILE* f) {
    int w = 80;
    if (::isatty(fileno(f)))
        if (const int tw = maya::detail::detect_terminal_width(); tw > 0) w = std::clamp(tw, 50, 110);
    return w;
}

void emit(const Element& e, FILE* f = stdout) {
    const int w = width_of(f);
    std::string s = painted(f) ? maya::render_to_string_ansi(e, w) : maya::render_to_string(e, w);
    // Rendered rows are padded to the width; trailing blanks only get in the
    // way of a copy. In styled output the padding sits before closing resets.
    std::string out;
    for (std::size_t at = 0; at < s.size();) {
        auto nl = s.find('\n', at);
        std::string line{std::string_view{s}.substr(at, nl == std::string::npos ? std::string::npos : nl - at)};
        for (;;) {
            if (line.ends_with(' ')) line.pop_back();
            else if (line.ends_with("\x1b[0m") && (line.size() == 4 || line[line.size() - 5] == ' ' || line.ends_with("\x1b[0m\x1b[0m")))
                line.resize(line.size() - 4);
            else break;
        }
        if (painted(f) && line.find('\x1b') != std::string::npos) line += "\x1b[0m";
        out += line;
        out += '\n';
        if (nl == std::string::npos) break;
        at = nl + 1;
    }
    while (out.ends_with("\n\n")) out.pop_back();
    std::fputs(out.c_str(), f);
}

Element blank() { return text(""); }
Element title(std::string s) { return text(std::move(s), ui::fg_bold(ui::accent)); }
Element heading(std::string s) { return text(std::move(s), ui::fg_bold(ui::fg)); }
Element dim(std::string s) { return text(std::move(s), ui::fg_dim(ui::muted)); }
Element ok(std::string s) { return text(std::move(s), ui::fg_bold(ui::status_ok)); }
Element warn(std::string s) { return text(std::move(s), ui::fg_bold(ui::status_warn)); }
Element bad(std::string s) { return text(std::move(s), ui::fg_bold(ui::status_error)); }
Element code(std::string s) { return text(std::move(s), maya::Style{}.with_fg(ui::accent)); }

Element fixed(Element e, int w) {
    auto box = maya::hstack();
    box.width(maya::Dimension::fixed(w));
    box.min_width(maya::Dimension::fixed(w));
    box.max_width(maya::Dimension::fixed(w));
    return box(std::move(e));
}

// A label in a fixed gutter and its value wrapping beside it.
Element row(Element label, Element value, int gutter) {
    auto right = maya::hstack();
    right.grow();
    auto r = maya::hstack();
    return r(fixed(std::move(label), gutter), right(std::move(value)));
}

Element line(std::vector<Element> parts) {
    auto h = maya::hstack();
    h.gap(1);
    return h(std::move(parts));
}

Element indent(Element e, int n = 2) {
    auto h = maya::hstack();
    return h(fixed(text(""), n), std::move(e));
}

Element stack(std::vector<Element> rows) {
    auto v = maya::vstack();
    return v(std::move(rows));
}

const char* dot() { return "\xc2\xb7"; }   // ·

// ── help ─────────────────────────────────────────────────────────────────

int help() {
    constexpr int g = 26;
    auto cmd = [&](std::string name, std::string what) { return row(indent(code(std::move(name))), text(std::move(what)), g); };
    emit(stack({
             line({title("agentty paste"), dim(dot()), dim("images, files and text into agentty, even over ssh and tmux")}),
             blank(),
             text("In the composer, Ctrl+V does all of this for you. These commands set it up and show what it sees."),
             blank(),
             heading("on the machine you type on"),
             cmd("setup", "add the ssh hook to ~/.ssh/config and start the agent (once)"),
             cmd("agent", "run the clipboard agent in the foreground"),
             blank(),
             heading("anywhere"),
             cmd("doctor", "show which routes paste would use, and try one"),
             cmd("paste [-o DIR]", "print the clipboard: text as is, images and files saved, their paths printed"),
             cmd("config", "list and set the paste settings (Settings \xe2\x86\x92 Paste)"),
             blank(),
             heading("when nothing reaches your clipboard (Terminal.app over ssh)"),
             cmd("howto", "one-time Mac or Windows setup to paste images as text"),
             cmd("page > paste.html", "a web page that turns a pasted image into one line of text"),
             cmd("encode FILE", "FILE as one line of text; paste it anywhere"),
             cmd("decode [DIR]", "turn such text on stdin back into files"),
             blank(),
             dim("Pasting an image as text also works in the browser, nothing to install: agentty.org/paste"),
         }),
         stderr);
    return 2;
}

// ── doctor ───────────────────────────────────────────────────────────────

std::string route_label(rp::Route r) {
    switch (r) {
        case rp::Route::socket: return "agent socket";
        case rp::Route::forward: return "ssh forward";
        case rp::Route::display: return "this machine";
        case rp::Route::tmux_buffer: return "tmux buffer";
        case rp::Route::terminal: return "terminal";
    }
    return "?";
}

std::string route_what(rp::Route r) {
    switch (r) {
        case rp::Route::socket: return "the one agent socket in the settings";
        case rp::Route::forward: return "the agent on your laptop, through ssh (after `setup`)";
        case rp::Route::display: return "this machine's own clipboard (Wayland, X11, macOS, Windows)";
        case rp::Route::tmux_buffer: return "tmux asks your terminal for text, we read its buffer";
        case rp::Route::terminal: return "ask your terminal (kitty: images and text; others: text)";
    }
    return "";
}

// What to do about a failed route, in a few plain words. Empty: nothing to add.
std::string route_hint(const rp::Error& e) {
    using rp::Code;
    if (!e.route) return {};
    switch (*e.route) {
        case rp::Route::forward:
            return e.code == Code::unreachable ? "run `agentty paste setup` on your laptop, then reconnect" : "";
        case rp::Route::display:
            return e.code == Code::no_display ? "no display here, that's normal over ssh" : "";
        case rp::Route::tmux_buffer:
            return e.code == Code::no_answer ? "add `set -g set-clipboard on` to tmux.conf, and allow clipboard reads in your terminal" : "";
        case rp::Route::terminal:
            if (e.code == Code::no_answer) return "your terminal doesn't answer clipboard reads; kitty, ghostty, wezterm and iTerm2 can";
            if (e.code == Code::refused) return "your terminal said no; allow clipboard reads in its settings";
            return "";
        case rp::Route::socket: return "";
    }
    return {};
}

std::string first_line(std::string_view s) { return std::string{s.substr(0, s.find('\n'))}; }

int doctor() {
    const auto d = rp::diagnose();
    const auto& c = d.config;
    constexpr int g = 16;
    std::vector<Element> rows;
    auto kv = [&](std::string k, Element v) { rows.push_back(row(indent(dim(std::move(k))), std::move(v), g)); };
    auto onoff = [](bool b) { return b ? std::string{"on"} : std::string{"off"}; };

    rows.push_back(line({title("agentty paste doctor")}));
    rows.push_back(blank());

    // Where we are decides the plan, so say that first.
    rows.push_back(heading("where you are"));
    std::string where = d.situation.ssh ? "over ssh" : "on this machine";
    if (d.tmux) where += d.situation.tmux ? ", in a tmux pane" : ", tmux set but not answering";
    kv("session", text(where));
    {
        using I = rp::Images;
        const auto im = d.situation.images;
        kv("terminal", im == I::yes ? ok("can send images (kitty)")
                     : im == I::no ? text("text only") : dim("unknown, asked when needed"));
    }
    if (d.situation.ssh && d.ssh_connection) kv("ssh", dim(*d.ssh_connection));
    std::string disp;
    if (d.wayland_display) disp += "wayland " + *d.wayland_display;
    if (d.x_display) disp += (disp.empty() ? "" : ", ") + std::string{"x11 "} + *d.x_display;
    if (disp.empty() && d.paste && d.paste->route == rp::Route::display) disp = d.paste->source + " (found, not in env)";
    kv("display", disp.empty() ? dim("none") : text(disp));
    {
        std::size_t live = std::ranges::count_if(d.forwards, [](auto& f) { return f.live; });
        std::string f = d.forwards.empty() ? "none" : std::format("{} live, {} stale", live, d.forwards.size() - live);
        kv("ssh forwards", d.forwards.empty() || live == 0 ? dim(f) : text(f));
    }
    kv("local agent", d.agent_alive ? ok("running") : dim("not running"));
    rows.push_back(blank());

    // The plan, with each route's outcome if this paste reached it.
    rows.push_back(heading("routes, in order"));
    if (d.plan.empty()) rows.push_back(indent(warn("every route is turned off in the settings")));
    bool reached_end = false;
    for (std::size_t i = 0; i < d.plan.size(); ++i) {
        const auto r = d.plan[i];
        Element mark = dim("\xc2\xb7");   // not tried
        std::vector<Element> detail;
        if (reached_end) {
            mark = dim("\xc2\xb7");
        } else if (d.paste && d.paste->route == r) {
            mark = ok("\xe2\x9c\x93");   // ✓
            reached_end = true;
        } else if (!d.paste) {
            if (const auto* e = d.paste.error().from(r)) {
                const bool final = d.paste.error().route == r;
                mark = final ? bad("\xe2\x9c\x97") : warn("\xe2\x80\x93");   // ✗ / –
                detail.push_back(indent(dim(first_line(e->what)), g + 2));
                if (auto h = route_hint(*e); !h.empty()) detail.push_back(indent(text("\xe2\x86\x92 " + h), g + 2));
                if (final) reached_end = true;
            }
        }
        rows.push_back(row(indent(line({std::move(mark), text(route_label(r))})), dim(route_what(r)), g + 2));
        for (auto& x : detail) rows.push_back(std::move(x));
    }
    rows.push_back(blank());

    // The result.
    if (d.paste) {
        rows.push_back(line({ok("\xe2\x9c\x93 paste works"), dim(std::string{"via "} + d.paste->source)}));
        for (auto& it : d.paste->items) {
            auto data = rp::payload(it);
            std::string what = std::visit(rp::overloaded{
                                              [](const rp::Text& t) { return std::format("text, {} chars", t.str().size()); },
                                              [](const rp::Image& m) { return std::string{m.mime.str()}; },
                                              [](const rp::File& f) { return std::format("file {}", f.name.str()); },
                                              [](const auto&) { return std::string{"item"}; },
                                          },
                                          it);
            rows.push_back(indent(line({text(what), dim(dot()), text(rp::human_size(data.size())), dim(dot()),
                                        dim("sha256 " + rp::hex(rp::sha256(data)).substr(0, 12))})));
        }
    } else {
        const auto& e = d.paste.error();
        const bool empty = e.code == rp::Code::empty;
        rows.push_back(empty ? line({warn("clipboard is empty"), dim("copy something and run this again")})
                             : line({bad("\xe2\x9c\x97 paste can't reach your clipboard")}));
        if (!empty && e.causes.empty()) rows.push_back(indent(text(first_line(e.what))));
        if (e.code == rp::Code::no_route || e.code == rp::Code::needs_terminal) {
            rows.push_back(blank());
            rows.push_back(text(d.situation.ssh ? "Fix: run `agentty paste setup` on your laptop and reconnect. For images "
                                                  "with nothing installed, copy them from agentty.org/paste."
                                                : "Fix: run `agentty paste setup` on the machine you type on."));
        }
    }
    rows.push_back(blank());

    // Settings, compact, last.
    rows.push_back(heading("settings"));
    kv("routes", dim(std::format("forwards {} {} display {} {} server display {} {} terminal {}", onoff(c.forwards), dot(),
                                 onoff(c.display), dot(), onoff(c.server_display), dot(),
                                 rp::terminal_mode_name(c.terminal))));
    kv("max size", dim(rp::human_size(c.max_bytes)));
    if (!c.socket.empty()) kv("socket", text(c.socket.string() + " (the only route)"));
    kv("saves to", dim(d.save_dir.string()));
    kv("agent", dim(d.agent));
    rows.push_back(dim("change them: agentty paste config, or Settings \xe2\x86\x92 Paste"));

    emit(stack(std::move(rows)));
    return d.paste || d.paste.error().code == rp::Code::empty ? 0 : 1;
}

// ── setup ────────────────────────────────────────────────────────────────

int setup() {
    auto r = rp::setup();
    if (!r) {
        emit(stack({line({bad("\xe2\x9c\x97 setup failed")}), indent(text(r.error().what))}), stderr);
        return 1;
    }
    std::vector<Element> rows;
    using C = rp::SetupReport::Change;
    switch (r->change) {
        case C::unchanged:
            rows.push_back(line({ok("\xe2\x9c\x93"), text("the ssh hook is already in"), code(r->ssh_config.string())}));
            break;
        case C::updated:
            rows.push_back(line({ok("\xe2\x9c\x93"), text("updated the ssh hook in"), code(r->ssh_config.string())}));
            break;
        case C::added:
            rows.push_back(line({ok("\xe2\x9c\x93"), text("added the ssh hook to the top of"), code(r->ssh_config.string())}));
            break;
    }
    if (r->change != C::unchanged) {
        rows.push_back(blank());
        std::string_view s = r->snippet;
        while (!s.empty()) {
            auto nl = s.find('\n');
            rows.push_back(indent(dim(std::string{s.substr(0, nl)}), 4));
            if (nl == std::string_view::npos) break;
            s.remove_prefix(nl + 1);
        }
    }
    rows.push_back(line({ok("\xe2\x9c\x93"), text("the clipboard agent is running")}));
    rows.push_back(blank());
    rows.push_back(text("New ssh sessions now carry your clipboard. Reconnect, then Ctrl+V in agentty pastes "
                        "images, files and text from this machine."));
    rows.push_back(dim("The server needs agentty too. Nothing else to set up there. Check it with "
                       "`agentty paste doctor` on the server."));
    emit(stack(std::move(rows)));
    return 0;
}

// ── config ───────────────────────────────────────────────────────────────

// Accepts "max_mb" or "paste.max_mb".
const reg::SettingDef* find_row(std::string_view key) {
    std::string id{key};
    if (!id.starts_with("paste.")) id = "paste." + id;
    const auto* d = reg::find(id);
    return d && d->owner() == reg::Owner::Paste ? d : nullptr;
}

void list(const paste_cfg::Config& c, const reg::EnvSnapshot& env) {
    std::vector<Element> rows;
    rows.push_back(line({title("paste settings"), dim(dot()), dim("settings.json \"paste\", also in Settings \xe2\x86\x92 Paste")}));
    rows.push_back(blank());
    std::size_t key_w = 0;
    for (const auto& d : reg::kSettings)
        if (d.owner() == reg::Owner::Paste) key_w = std::max(key_w, d.id.size() - 6);
    const int g = static_cast<int>(key_w) + 4;
    for (const auto& d : reg::kSettings) {
        if (d.owner() != reg::Owner::Paste) continue;
        std::string val = reg::get(c, d);
        if (val.empty()) val = "\"\"";
        Element from = dim("default");
        if (auto e = reg::env_override(d, env); !e.empty()) from = warn("from " + e);
        else if (!reg::is_default(c, d)) from = text("set in settings.json");
        rows.push_back(row(indent(code(std::string{d.id.substr(6)})), line({text(val, ui::fg_bold(ui::fg)), dim(dot()), std::move(from)}), g));
        rows.push_back(row(text(""), dim(std::string{d.help}), g));
    }
    rows.push_back(blank());
    rows.push_back(row(dim("get"), code("agentty paste config get <key>"), 7));
    rows.push_back(row(dim("set"), code("agentty paste config set <key> <value>"), 7));
    rows.push_back(row(dim("reset"), code("agentty paste config reset <key|all>"), 7));
    rows.push_back(row(dim("env"), dim("wins over settings.json"), 7));
    std::size_t env_w = 0;
    for (const auto& d : reg::kSettings)
        if (d.owner() == reg::Owner::Paste) env_w = std::max(env_w, d.env.size());
    for (const auto& d : reg::kSettings)
        if (d.owner() == reg::Owner::Paste)
            rows.push_back(indent(row(code(std::string{d.env}), dim(std::string{d.id.substr(6)}), static_cast<int>(env_w) + 2), 7));
    emit(stack(std::move(rows)));
}

int fail(std::string msg) {
    emit(line({bad("agentty paste:"), text(std::move(msg))}), stderr);
    return 1;
}

int config_usage() {
    emit(stack({
             line({title("agentty paste config")}),
             indent(code("agentty paste config")),
             indent(code("agentty paste config get <key>")),
             indent(code("agentty paste config set <key> <value>")),
             indent(code("agentty paste config reset <key|all>")),
         }),
         stderr);
    return 2;
}

int config(Io io, const maya::unlocked& cap, std::span<const std::string> args) {
    auto s = persistence::load_settings(io);
    const auto env = reg::read_env(io);
    if (args.empty() || args[0] == "list") {
        list(s.paste, env);
        return 0;
    }
    const std::string& verb = args[0];
    if (verb == "get" && args.size() == 2) {
        const auto* d = find_row(args[1]);
        if (!d) return fail("there is no setting " + args[1] + " (see `agentty paste config`)");
        std::printf("%s\n", reg::get(s.paste, *d).c_str());   // plain: scripts read this
        return 0;
    }
    if (verb == "set" && (args.size() == 3 || args.size() == 2)) {
        const auto* d = find_row(args[1]);
        if (!d) return fail("there is no setting " + args[1] + " (see `agentty paste config`)");
        const std::string value = args.size() == 3 ? args[2] : std::string{};
        if (!reg::set(s.paste, *d, value)) {
            std::string m = std::string{d->id.substr(6)} + " can't be \"" + value + "\"";
            if (!d->options.empty()) m += " (one of " + std::string{d->options} + ")";
            return fail(std::move(m));
        }
        persistence::save_settings(io, cap, s);
        std::vector<Element> rows{line({ok("\xe2\x9c\x93"), code(std::string{d->id.substr(6)}), text("= " + reg::get(s.paste, *d))})};
        if (auto e = reg::env_override(*d, env); !e.empty())
            rows.push_back(indent(warn(e + " is set, so it still wins over this")));
        emit(stack(std::move(rows)));
        return 0;
    }
    if (verb == "reset" && args.size() == 2) {
        if (args[1] == "all") {
            s.paste = paste_cfg::Config{};
        } else {
            const auto* d = find_row(args[1]);
            if (!d) return fail("there is no setting " + args[1] + " (see `agentty paste config`)");
            reg::reset(s.paste, *d);
        }
        persistence::save_settings(io, cap, s);
        emit(line({ok("\xe2\x9c\x93"), text("reset"), code(args[1])}));
        return 0;
    }
    return config_usage();
}

}  // namespace

int run(Io io, const maya::unlocked& cap, std::span<const std::string> args) {
    if (!args.empty() && args[0] == "config") return config(io, cap, args.subspan(1));
    clipboard_configure(persistence::load_settings(io).paste);
    if (!args.empty()) {
        const auto& c = args[0];
        if (c == "help" || c == "-h" || c == "--help") return help();
        if (c == "doctor" && args.size() == 1) return doctor();
        if (c == "setup" && args.size() == 1) return setup();
    }
    return clipboard_cli(args);
}

}  // namespace agentty::paste_cli
