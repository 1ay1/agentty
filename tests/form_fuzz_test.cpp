// form_fuzz_test — random keystrokes against every field kind, with the
// form's invariants checked after EVERY key.
//
// Unit tests pin the bugs we already know (#97, the deaf 0-8 row, the
// swallowed letter). This hunts the ones we don't: thousands of random
// sessions mixing navigation, typing, deletion, caret moves, paste, ^U and
// dropdowns over a form with one row of each kind. After every key:
//
//   1. text-like values are valid UTF-8 and the caret sits on a code-point
//      boundary inside the value (no split characters, no out-of-range caret);
//   2. no control characters ever land in a single-line value;
//   3. a Number row's committed value is inside [min, max], its draft is
//      digits only, and the draft is live exactly while the row is edited;
//   4. Editing focus only ever sits on a text-like, editable row;
//   5. a dropdown is only open on a Choice row, with its highlight in range;
//   6. the cursor is in range and never parked on a header;
//   7. a slider is inside [min, max] and a choice index inside its labels;
//   8. caret_offset() points inside the rendered text.
//
// A failure prints the seed and the key history so it reproduces exactly.

#include "agtest.hpp"

#include <maya/terminal/input.hpp>

#include "agentty/runtime/panel/form.hpp"
#include "agentty/runtime/panel/form_keys.hpp"

#include <cstdint>
#include <cstdlib>
#include <random>
#include <sstream>
#include <string>
#include <vector>

using namespace agentty::form;
namespace keys = agentty::form::keys;

namespace {

Form all_kinds() {
    return Builder{"Fuzz"}
        .header("Section A")
        .choice("choice", "Choice", {"Alpha", "Beta", "Gamma"}, {"a", "b", "g"}, "b")
        .text("text", "Text", "h\xc3\xa9llo \xe6\xbc\xa2")     // héllo 漢
        .number("port", "Port", 11434, 1, 65535)
        .number("depth", "Depth", 3, 0, 8)
        .number("wide", "Wide", 0, 0, 1000000000000LL)
        .header("Section B")
        .slider("weight", "Weight", 0.65, 0.0, 1.0, 0.05)
        .secret("key", "Key", "sk-secret")
        .toggle("flag", "Flag", true)
        .path("path", "Path", "/tmp")
        .action("go", "Go")
        .build();
}

bool valid_utf8(const std::string& s) {
    std::size_t i = 0;
    while (i < s.size()) {
        const auto c = static_cast<unsigned char>(s[i]);
        int n = c < 0x80 ? 1 : (c >> 5) == 0x6 ? 2 : (c >> 4) == 0xE ? 3 : (c >> 3) == 0x1E ? 4 : 0;
        if (n == 0 || i + n > s.size()) return false;
        std::uint32_t cp = n == 1 ? c : n == 2 ? (c & 0x1F) : n == 3 ? (c & 0x0F) : (c & 0x07);
        for (int k = 1; k < n; ++k) {
            const auto cc = static_cast<unsigned char>(s[i + k]);
            if ((cc & 0xC0) != 0x80) return false;
            cp = (cp << 6) | (cc & 0x3F);
        }
        // Overlong, surrogate and out-of-range code points are invalid.
        if ((n == 2 && cp < 0x80) || (n == 3 && cp < 0x800) || (n == 4 && cp < 0x10000)) return false;
        if (cp >= 0xD800 && cp <= 0xDFFF) return false;
        if (cp > 0x10FFFF) return false;
        i += n;
    }
    return true;
}

bool on_boundary(const std::string& s, std::size_t pos) {
    if (pos > s.size()) return false;
    if (pos == s.size()) return true;
    return (static_cast<unsigned char>(s[pos]) & 0xC0) != 0x80;
}

bool has_control(const std::string& s) {
    for (unsigned char c : s)
        if (c < 0x20 || c == 0x7F) return true;
    return false;
}

maya::KeyEvent special(maya::SpecialKey k) { maya::KeyEvent e; e.key = k; return e; }
maya::KeyEvent chr(char32_t c, bool ctrl = false) {
    maya::KeyEvent e; e.key = maya::CharKey{c}; e.mods.ctrl = ctrl; return e;
}

struct Step { std::string what; };

// Check every invariant; on failure returns a description.
std::string check(const Form& f) {
    const int n = static_cast<int>(f.fields.size());
    if (n > 0 && (f.cursor < 0 || f.cursor >= n)) return "cursor out of range";
    if (n > 0 && f.fields[static_cast<std::size_t>(f.cursor)].is_header()) return "cursor on a header";
    const Field* focused = f.focused();
    if (f.editing()) {
        if (!focused) return "editing with no focused row";
        if (!focused->is_text_like()) return "editing a non-text row";
        if (!focused->editable()) return "editing a non-editable row";
    }
    if (f.choosing()) {
        if (!focused || !focused->is_choice()) return "dropdown open on a non-choice row";
        const auto* d = f.dropdown();
        const int cnt = std::get<field::Choice>(focused->value).count();
        if (!d || d->highlighted < 0 || d->highlighted >= cnt) return "dropdown highlight out of range";
    }
    for (int i = 0; i < n; ++i) {
        const Field& row = f.fields[static_cast<std::size_t>(i)];
        const bool this_editing = f.editing() && i == f.cursor;
        std::string err = std::visit([&](const auto& v) -> std::string {
            using T = std::decay_t<decltype(v)>;
            if constexpr (std::is_same_v<T, field::Text> || std::is_same_v<T, field::Secret>
                       || std::is_same_v<T, field::Path>) {
                if (!valid_utf8(v.value)) return "invalid UTF-8 in " + row.id;
                if (has_control(v.value)) return "control char in " + row.id;
                if (!on_boundary(v.value, v.cursor)) return "caret mid-character in " + row.id;
            } else if constexpr (std::is_same_v<T, field::Number>) {
                if (v.value < v.min || v.value > v.max) return "number out of range in " + row.id;
                for (char c : v.draft) if (c < '0' || c > '9') return "non-digit in draft of " + row.id;
                if (v.editing != this_editing) return "draft live state wrong in " + row.id;
                if (!v.editing && !v.draft.empty()) return "stale draft in " + row.id;
                const auto live = number_value(v);
                if (live < v.min || live > v.max) return "live value out of range in " + row.id;
            } else if constexpr (std::is_same_v<T, field::Slider>) {
                if (v.value < v.min - 1e-9 || v.value > v.max + 1e-9) return "slider out of range in " + row.id;
            } else if constexpr (std::is_same_v<T, field::Choice>) {
                if (v.index < 0 || v.index >= v.count()) return "choice index out of range in " + row.id;
            }
            return {};
        }, row.value);
        if (!err.empty()) return err;
        if (this_editing && row.is_text_like()) {
            const auto at = caret_offset(row.value, true);
            const std::size_t len = std::visit([](const auto& v) -> std::size_t {
                using T = std::decay_t<decltype(v)>;
                if constexpr (std::is_same_v<T, field::Number>) return number_text(v).size();
                else if constexpr (requires { v.value.size(); }) return v.value.size();
                else return 0;
            }, row.value);
            if (at == std::string::npos || at > len) return "caret_offset outside text of " + row.id;
        }
    }
    return {};
}

// A random key, weighted toward what users actually press.
maya::KeyEvent random_key(std::mt19937& rng, std::string& label) {
    using S = maya::SpecialKey;
    static const S specials[] = {S::Up, S::Down, S::Left, S::Right, S::Enter, S::Escape,
                                 S::Backspace, S::Delete, S::Home, S::End, S::PageUp,
                                 S::PageDown, S::Tab};
    static const char32_t chars[] = {
        U'0', U'1', U'5', U'9', U'a', U'Z', U' ', U'-', U'/', U'.',
        U'\u00e9', U'\u6f22', U'\U0001F600',            // 2, 3, 4-byte UTF-8
        0x7F, 0x08, 0x09, 0x0A, 0x0D, 0x1B, 0x00,       // DEL, BS, TAB, LF, CR, ESC, NUL
        0xD800, 0xDFFF, 0x110000, 0xFFFF, 0x200B};      // surrogates, out of range
    std::uniform_int_distribution<int> kind(0, 9);
    const int k = kind(rng);
    if (k < 4) {
        const auto s = specials[std::uniform_int_distribution<std::size_t>(0, std::size(specials) - 1)(rng)];
        label = "special" + std::to_string(static_cast<int>(s));
        return special(s);
    }
    if (k == 4) {
        static const char32_t ctrls[] = {U'u', U'a', U'e', U'x', U'w', U'k'};
        const auto c = ctrls[std::uniform_int_distribution<std::size_t>(0, std::size(ctrls) - 1)(rng)];
        label = std::string{"^"} + static_cast<char>(c);
        return chr(c, true);
    }
    const auto c = chars[std::uniform_int_distribution<std::size_t>(0, std::size(chars) - 1)(rng)];
    label = "chr U+" + std::to_string(static_cast<std::uint32_t>(c));
    return chr(c);
}

std::string random_paste(std::mt19937& rng) {
    static const char* pieces[] = {"8080", "abc", "\n", "\t", "x\ny", "12\r\n34",
                                   "\xc3\xa9", "\xe6\xbc\xa2", "\xff\xfe", "\xc3",
                                   "\xed\xa0\x80", "\x01\x7f", "999999999999999999999",
                                   "", "  spaced  "};
    std::string s;
    const int parts = std::uniform_int_distribution<int>(1, 3)(rng);
    for (int i = 0; i < parts; ++i)
        s += pieces[std::uniform_int_distribution<std::size_t>(0, std::size(pieces) - 1)(rng)];
    return s;
}

}  // namespace

TEST_CASE("form fuzz: random sessions keep every invariant") {
    // FORM_FUZZ_SESSIONS / FORM_FUZZ_KEYS scale it up for a deep run.
    const auto env_int = [](const char* k, int d) {
        const char* v = std::getenv(k);
        return v ? std::max(1, std::atoi(v)) : d;
    };
    const int kSessions = env_int("FORM_FUZZ_SESSIONS", 3000);
    const int kKeys = env_int("FORM_FUZZ_KEYS", 120);
    int failures = 0;
    for (int seed = 1; seed <= kSessions && failures < 5; ++seed) {
        std::mt19937 rng(static_cast<std::uint32_t>(seed));
        Form f = all_kinds();
        f.cursor = 1;   // first real row
        std::vector<std::string> history;
        for (int step = 0; step < kKeys; ++step) {
            std::string label;
            if (std::uniform_int_distribution<int>(0, 24)(rng) == 0) {
                const std::string text = random_paste(rng);
                label = "paste";
                if (std::uniform_int_distribution<int>(0, 1)(rng)) (void)paste_into_focused(f, text);
                else                                                (void)paste_into(f, text);
            } else {
                const auto ev = random_key(rng, label);
                if (auto a = keys::translate(f, ev)) (void)keys::apply(f, *a);
            }
            history.push_back(label);
            const std::string err = check(f);
            if (!err.empty()) {
                std::ostringstream os;
                os << "seed " << seed << " step " << step << ": " << err << "\n  keys:";
                for (const auto& h : history) os << ' ' << h;
                FAIL_CHECK(os.str());
                ++failures;
                break;
            }
        }
    }
    CHECK(failures == 0);
}

// ── The same, through every real settings pane ──────────────────────────────
//
// Keys go the way the app sends them (on_key -> update), so pane code runs
// too: rebuilds, saves, pickers, the sandbox list editor, Esc back out. The
// form invariants are checked on whichever form is open after every key, and
// the app must never crash or leave a pane holding a stale edit.

#include "agentty/runtime/app/deps.hpp"
#include "agentty/runtime/app/subscribe.hpp"
#include "agentty/runtime/app/update.hpp"
#include "agentty/runtime/panel/rag.hpp"
#include "agentty/runtime/panel/top.hpp"

namespace {

using namespace agentty;
namespace pn = agentty::ui::panel;

store::Settings g_fuzz_settings;

void fuzz_stub_deps() {
    app::install_deps(app::Deps{
        .stream         = [](const maya::unlocked&, auto, auto) {},
        .save_thread    = [](const Thread&) {},
        .delete_thread  = [](const auto&) {},
        .load_threads   = [] { return std::vector<Thread>{}; },
        .load_thread    = [](const ThreadId&) { return std::optional<Thread>{}; },
        .load_settings  = [] { return g_fuzz_settings; },
        .save_settings  = [](const store::Settings& s) { g_fuzz_settings = s; },
        .new_thread_id  = [] { return ThreadId{"t-fuzz"}; },
        .title_from     = [](std::string_view t) { return std::string{t}; },
        .auth           = {},
    });
}

// The form of whatever settings pane is on top, if any.
const Form* open_form(const Model& m) {
    if (auto* o = m.ui.panel.get<pn::Rag>())         return &o->embed.form;
    if (auto* o = m.ui.panel.get<pn::SmartMode>())   return &o->form;
    if (auto* o = m.ui.panel.get<pn::WebSearch>())   return &o->form;
    if (auto* o = m.ui.panel.get<pn::Appearance>())  return &o->pane.form;
    if (auto* o = m.ui.panel.get<pn::Sandbox>())     return &o->pane.form;
    if (auto* o = m.ui.panel.get<pn::SandboxList>()) return &o->pane.form;
    if (auto* o = m.ui.panel.get<pn::PluginEdit>())  return &o->form;
    return nullptr;
}

}  // namespace

TEST_CASE("form fuzz: random keys through every real settings pane") {
    fuzz_stub_deps();
    const char* env = std::getenv("FORM_FUZZ_SESSIONS");
    const int sessions = env ? std::max(1, std::atoi(env) / 20) : 150;
    const std::vector<Msg> opens = {
        Msg{OpenAppearance{}}, Msg{OpenSandbox{}}, Msg{OpenSmartMode{}},
        Msg{OpenRag{}},        Msg{OpenWebSearch{}},
        Msg{OpenSettingsList{settings::Category::General}},
    };
    int failures = 0;
    for (int seed = 1; seed <= sessions && failures < 5; ++seed) {
        std::mt19937 rng(static_cast<std::uint32_t>(seed * 7919));
        const auto& open = opens[static_cast<std::size_t>(seed) % opens.size()];
        Model start;
        if (seed % 2) {
            start.d.persisted.sandbox.configured = true;
            start.d.persisted.rag.embed_backend = "ollama";
        }
        Model m = app::update(std::move(start), open).first;
        std::vector<std::string> history;
        for (int step = 0; step < 150; ++step) {
            std::string label;
            if (std::uniform_int_distribution<int>(0, 29)(rng) == 0) {
                label = "paste";
                m = app::update(std::move(m), app::on_paste(m, maya::PasteEvent{random_paste(rng)})).first;
            } else {
                const auto ev = random_key(rng, label);
                if (auto msg = app::on_key(m, ev))
                    m = app::update(std::move(m), std::move(*msg)).first;
            }
            history.push_back(label);
            std::string err;
            if (const Form* f = open_form(m)) err = check(*f);
            if (!err.empty()) {
                std::ostringstream os;
                os << "pane seed " << seed << " step " << step << ": " << err << "\n  keys:";
                for (const auto& h : history) os << ' ' << h;
                FAIL_CHECK(os.str());
                ++failures;
                break;
            }
            // Everything closed: reopen, so the session keeps exercising.
            if (ui::panel::top(m) == ui::panel::Kind::None)
                m = app::update(std::move(m), Msg{open}).first;
        }
    }
    CHECK(failures == 0);
}
