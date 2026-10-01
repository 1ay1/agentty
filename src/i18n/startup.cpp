// agentty::i18n — picking a language at startup, and the embedded catalogs.
//
// Separate from i18n.cpp because that file is pure storage and lookup: it has
// no idea where catalogs come from or how a language gets chosen. This one
// owns both, which keeps the testable core free of getenv and file IO.

#include "agentty/i18n/startup.hpp"

#include "catalog_de.hpp"
#include "catalog_es.hpp"
#include "catalog_fr.hpp"
#include "catalog_ja.hpp"
#include "catalog_pt_BR.hpp"
#include "catalog_ru.hpp"
#include "catalog_zh_CN.hpp"

#include <cstdlib>

namespace agentty::i18n {

namespace {

// The English catalog, embedded.
//
// Embedded rather than read from disk, and that is not just about shipping
// one file. A catalog read at runtime is an injection surface: it decides
// what every label says, including the ones on a permission prompt. A user
// who can be talked into dropping a JSON file in the right directory can be
// shown a dialog that says something other than what it does.
//
// Debug builds hot-reload from $AGENTTY_HOME/i18n/ (see load_overrides) so a
// translator can iterate, and that path is debug-only for exactly this
// reason.
//
// STARTING SMALL AND HONEST. These are the sandbox pane's rows -- the
// densest user-facing surface in the app and the best-tested one. The sweep
// continues pane by pane; the lint (tests/lint/i18n_lint.cmake) is what
// proves nothing was dropped on the way.
constexpr std::string_view kEnglish = R"JSON({
  "_meta": { "lang": "en", "plurals": ["one", "other"] },

  "lang.picker.title":   { "text": "Language",      "width_hint": 8 },
  "lang.picker.help":    { "text": "agentty's own labels · the model answers in whatever language you write in", "width_hint": 72 },
  "lang.auto":           { "text": "Auto (follow system)", "width_hint": 20 },
  "lang.incomplete":     { "text": "{pct}% translated",    "width_hint": 16 },

  "appearance.theme":        { "text": "Theme",               "width_hint": 5 },
  "appearance.scheme":       { "text": "Scheme",              "width_hint": 6 },
  "appearance.color":        { "text": "Color",               "width_hint": 5 },
  "appearance.layout":       { "text": "Layout",              "width_hint": 6 },
  "appearance.prose_width":  { "text": "Prose width",         "width_hint": 11 },
  "appearance.compact":      { "text": "Compact turns",       "width_hint": 13 },
  "appearance.motion":       { "text": "Motion",              "width_hint": 6 },
  "appearance.content":      { "text": "Content",             "width_hint": 7 },
  "appearance.syntax":       { "text": "Syntax highlighting", "width_hint": 19 },

  "appearance.help.scheme":      { "text": "native keeps your terminal's own colors · Enter to browse", "width_hint": 56 },
  "appearance.help.prose_width": { "text": "wrap assistant text at N columns · 0 = the full width",    "width_hint": 53 },
  "appearance.help.compact":     { "text": "drop the blank line between turns",   "width_hint": 33 },
  "appearance.help.syntax":      { "text": "colour code fences by language",      "width_hint": 30 },

  "sandbox.read_paths":  { "text": "Also readable", "width_hint": 13 },
  "sandbox.write_paths": { "text": "Also writable", "width_hint": 13 },
  "sandbox.deny_paths":  { "text": "Masked",        "width_hint": 6 },
  "sandbox.ports":       { "text": "Allowed ports", "width_hint": 13 },
  "sandbox.memory":      { "text": "Memory (MB)",   "width_hint": 11 },
  "sandbox.procs":       { "text": "Processes",     "width_hint": 9 },
  "sandbox.scan_depth":  { "text": "Secret scan depth", "width_hint": 17 },

  "sandbox.help.read_paths":  { "text": "extra paths a command may read, beyond the scope above · for a dependency outside the workspace", "width_hint": 94 },
  "sandbox.help.write_paths": { "text": "separate from readable on purpose: granting write is a different decision", "width_hint": 72 },
  "sandbox.help.deny_paths":  { "text": "carved out even inside the scope above · e.g. ~/.cargo/credentials. credential files are masked anyway.", "width_hint": 102 },
  "sandbox.help.ports":       { "text": "443 https · 80 http · 22 git-ssh · 53 dns. forgetting 53 breaks everything.", "width_hint": 74 },
  "sandbox.help.memory":      { "text": "0 = no cap", "width_hint": 10 },
  "sandbox.help.procs":       { "text": "0 = no cap · stops fork bombs", "width_hint": 29 },
  "sandbox.help.scan_depth":  { "text": "0 = workspace root only · 3 finds services/*/.env · each level costs stat() calls on every command", "width_hint": 97 },

  "common.on":     { "text": "on",     "width_hint": 2 },
  "common.off":    { "text": "off",    "width_hint": 3 },
  "common.custom": { "text": "Custom", "width_hint": 17 }
})JSON";
// common.custom's width_hint is 17, not the English string's 6, and that is
// the general rule rather than an exception: a width_hint is the column
// count the LAYOUT must absorb, not the length of the English. German needs
// 17 for "Benutzerdefiniert", which has no shorter form that is still the
// word. The lint caught it at 2.83x over budget -- and the English hint was
// what was wrong, because a one-word English label is no evidence that every
// language has one.

// Read a locale from the environment, in POSIX precedence order.
//
// LC_ALL overrides everything, LC_MESSAGES is the one that actually governs
// program messages, LANG is the fallback. LANGUAGE (a GNU extension) is a
// colon-separated PREFERENCE LIST -- we take its first entry, which is the
// closest thing to "what this user would rather read".
[[nodiscard]] bool from_environment(Lang& out) {
    const char* const vars[] = {"LC_ALL", "LC_MESSAGES", "LANGUAGE", "LANG"};
    for (const char* v : vars) {
        const char* raw = std::getenv(v);
        if (!raw || !*raw) continue;

        std::string_view s{raw};
        // "C" and "POSIX" mean "no locale", not "a language named C". They
        // must not fall through to a prefix match that finds Czech.
        if (s == "C" || s == "POSIX" || s == "C.UTF-8") return false;

        // LANGUAGE is a list: "de:en:fr". Take the first.
        if (const auto colon = s.find(':'); colon != std::string_view::npos)
            s = s.substr(0, colon);

        if (parse_tag(s, out)) return true;
        // A tag we recognise as a language but do not SHIP (ar, he, th) must
        // stop here rather than falling through to the next variable --
        // LANG=ar_EG with LC_MESSAGES unset means Arabic, and answering
        // "English" is right while answering "whatever LANG says next" is
        // not. parse_tag already returned false; the loop continuing is the
        // bug this comment exists to prevent, so: do not add a `continue`
        // that skips a VALID-but-unshipped tag.
        return false;
    }
    return false;
}

}  // namespace

Lang resolve_language(std::string_view cli_lang, std::string_view saved) {
    Lang l{};

    // 1. The flag. A command always wins -- it is what the user typed THIS
    //    run, and it must not be second-guessed by a setting or a locale.
    if (!cli_lang.empty() && parse_tag(cli_lang, l)) return l;

    // 2. The saved setting, when the user has made a choice. Empty means
    //    "auto", which is why it is stored empty rather than as "en" (see
    //    ui_prefs::Prefs::lang).
    if (!saved.empty() && parse_tag(saved, l)) return l;

    // 3. The environment.
    if (from_environment(l)) return l;

    // 4. English.
    return Lang::en;
}

bool init(std::string_view cli_lang, std::string_view saved) {
    // English ALWAYS loads, whatever the choice. It is the fallback for a
    // catalog that is only 70% translated, and completeness() measures
    // against it -- so a missing en catalog would make every other language
    // report 0% and fall back to raw ids.
    if (!install_catalog(Lang::en, kEnglish)) return false;

    // Every OTHER catalog, unconditionally.
    //
    // Loading all of them rather than just the active one costs a parse per
    // language at startup (microseconds -- these are a few dozen short
    // strings) and buys two things worth more than that: the picker can show
    // real completeness percentages for languages you are not using, and
    // switching language at runtime is a pointer store rather than a parse.
    //
    // Failures are IGNORED on purpose. A malformed translation must not stop
    // agentty from starting; install_catalog leaves the slot empty,
    // set_active refuses it, and the picker shows it at 0%. That is visible
    // and recoverable, which a startup abort is not.
    (void)install_catalog(Lang::de, catalogs::kGerman);
    (void)install_catalog(Lang::zh_CN, catalogs::kChineseSimplified);
    (void)install_catalog(Lang::es, catalogs::kSpanish);
    (void)install_catalog(Lang::fr, catalogs::kFrench);
    (void)install_catalog(Lang::ja, catalogs::kJapanese);
    (void)install_catalog(Lang::pt_BR, catalogs::kPortugueseBR);
    (void)install_catalog(Lang::ru, catalogs::kRussian);

    const Lang want = resolve_language(cli_lang, saved);
    if (want == Lang::en) return set_active(Lang::en);

    // A language with no catalog compiled in is not an error: it is a
    // language nobody has translated yet. set_active refuses and leaves
    // English in place, which is the right outcome and needs no message --
    // the picker shows completeness so the gap is visible where it matters.
    if (!set_active(want)) return set_active(Lang::en);
    return true;
}

}  // namespace agentty::i18n
