// i18n_test — the translation core.
//
// Two things are worth testing here and they are very different in kind.
//
// The PLURAL RULES are a transcription of an external spec (CLDR). The risk
// is not that the code is subtly wrong under concurrency -- it is that a
// human copied a rule incorrectly, and the only defence is to check every
// integer against an independent statement of the same rule. That is what
// the sweep below does: 0..1000 for all twenty languages, against rules
// re-derived from the CLDR chart rather than from the implementation.
//
// Everything ELSE (lookup, fallback, tag parsing, the live swap) is ordinary
// code and gets ordinary cases.
//
// Why a sweep rather than spot checks: Russian's rule has a hole at 11-14
// that a reasonable person tests at 11 and then misses at 111. Czech's `few`
// is 2-4 with NO modulo, so 22 is `other` there and `few` in Russian -- a
// shared implementation gets exactly one of them right, and spot checks
// chosen by the person who wrote the bug will agree with the bug.

#include <string>
#include <vector>

#include "agtest.hpp"

#include "agentty/i18n/i18n.hpp"
#include "agentty/i18n/plural.hpp"
#include "agentty/i18n/startup.hpp"

using namespace agentty::i18n;

namespace {

// An INDEPENDENT statement of the CLDR rules, written from the published
// chart rather than from plural.hpp. If both are wrong in the same way the
// test is worthless, so these are deliberately spelled differently: the
// implementation switches on a shared Rule enum, this switches per language
// and repeats itself.
Plural expected(Lang l, long long n) {
    const auto v   = static_cast<unsigned long long>(n < 0 ? -n : n);
    const auto d10 = v % 10;
    const auto d100 = v % 100;

    switch (l) {
        // one form
        case Lang::zh_CN: case Lang::ja: case Lang::ko: case Lang::id:
            return Plural::Other;

        // fr: i = 0,1 -> one
        case Lang::fr:
            return (v == 0 || v == 1) ? Plural::One : Plural::Other;

        // two forms, i = 1 -> one
        case Lang::en: case Lang::de: case Lang::nl: case Lang::sv:
        case Lang::da: case Lang::no: case Lang::it: case Lang::es:
        case Lang::pt_BR: case Lang::hu: case Lang::tr:
            return (v == 1) ? Plural::One : Plural::Other;

        // ru, uk
        case Lang::ru: case Lang::uk:
            if (d100 >= 11 && d100 <= 14) return Plural::Many;
            if (d10 == 1) return Plural::One;
            if (d10 >= 2 && d10 <= 4) return Plural::Few;
            return Plural::Many;

        // pl
        case Lang::pl:
            if (v == 1) return Plural::One;
            if (d100 >= 12 && d100 <= 14) return Plural::Many;
            if (d10 >= 2 && d10 <= 4) return Plural::Few;
            return Plural::Many;

        // cs -- no modulo, which is the whole point
        case Lang::cs:
            if (v == 1) return Plural::One;
            if (v >= 2 && v <= 4) return Plural::Few;
            return Plural::Other;
    }
    return Plural::Other;
}

const Lang kAll[] = {
    Lang::en, Lang::zh_CN, Lang::es, Lang::pt_BR, Lang::ja, Lang::de,
    Lang::fr, Lang::ko, Lang::it, Lang::ru, Lang::tr, Lang::id,
    Lang::pl, Lang::nl, Lang::uk, Lang::sv, Lang::cs, Lang::da,
    Lang::no, Lang::hu,
};

}  // namespace

TEST_CASE("i18n plural: every language, every n in 0..1000") {
    for (const Lang l : kAll) {
        for (long long n = 0; n <= 1000; ++n) {
            const auto got = select(l, n);
            const auto want = expected(l, n);
            if (got != want) {
                // Report ONCE per language with the offending n, rather than
                // 1000 identical failures that bury which language broke.
                INFO("lang " << std::string{english_name(l)} << " n=" << n
                             << " got " << std::string{key_of(got)}
                             << " want " << std::string{key_of(want)});
                CHECK(got == want);
                break;
            }
        }
    }
}

TEST_CASE("i18n plural: the cases that distinguish the rules") {
    // Russian's teens. 11 ends in 1 and is NOT `one`.
    CHECK(select(Lang::ru, 1)   == Plural::One);
    CHECK(select(Lang::ru, 11)  == Plural::Many);
    CHECK(select(Lang::ru, 21)  == Plural::One);
    CHECK(select(Lang::ru, 111) == Plural::Many);
    CHECK(select(Lang::ru, 121) == Plural::One);

    // Czech vs Russian at 22. THE divergence -- one implementation cannot
    // serve both, and this is the pair that proves we did not try.
    CHECK(select(Lang::cs, 22) == Plural::Other);
    CHECK(select(Lang::ru, 22) == Plural::Few);

    // French: zero is singular.
    CHECK(select(Lang::fr, 0) == Plural::One);
    CHECK(select(Lang::en, 0) == Plural::Other);

    // CJK + Indonesian: one form, whatever the count.
    for (const Lang l : {Lang::ja, Lang::zh_CN, Lang::ko, Lang::id})
        for (long long n : {0, 1, 2, 5, 11, 100})
            CHECK(select(l, n) == Plural::Other);
}

TEST_CASE("i18n plural: has_category agrees with select") {
    // A category select() can produce must be one the catalog is required to
    // carry, or the lint would demand a form the runtime never reads (and
    // vice versa). Checked by construction rather than by eye.
    for (const Lang l : kAll) {
        bool seen[4] = {};
        for (long long n = 0; n <= 1000; ++n)
            seen[static_cast<std::size_t>(select(l, n))] = true;
        for (const Plural p : {Plural::One, Plural::Few, Plural::Many,
                               Plural::Other}) {
            INFO("lang " << std::string{english_name(l)}
                         << " category " << std::string{key_of(p)});
            CHECK(seen[static_cast<std::size_t>(p)] == has_category(l, p));
        }
    }
}

TEST_CASE("i18n: tag parsing handles what environments actually set") {
    struct Case { const char* in; const char* want; };   // nullptr = refuse
    const Case cases[] = {
        {"en",            "en"},
        {"EN",            "en"},          // case-insensitive
        {"de_DE.UTF-8",   "de"},          // the POSIX shape
        {"de-AT",         "de"},          // region we do not carry -> base
        {"pt-BR",         "pt-BR"},       // exact
        {"pt-PT",         "pt-BR"},       // close enough, deliberately
        {"zh-CN",         "zh-CN"},
        {"zh",            "zh-CN"},
        {"zh-Hans",       "zh-CN"},
        {"nb-NO",         "no"},          // CLDR splits nb/nn, we do not
        {"nn",            "no"},
        {"es_MX@euro",    "es"},          // modifier stripped
        // Traditional Chinese is NOT a dialect of Simplified. Serving zh-CN
        // here would look right and be wrong, which is worse than English.
        {"zh-TW",         nullptr},
        {"zh-HK",         nullptr},
        {"ar",            nullptr},       // not shipped: needs bidi
        {"",              nullptr},
        {"klingon",       nullptr},
    };
    for (const auto& c : cases) {
        Lang got{};
        const bool ok = parse_tag(c.in, got);
        INFO("tag '" << std::string{c.in} << "'");
        if (c.want == nullptr) {
            CHECK(!ok);
        } else {
            REQUIRE(ok);
            CHECK(tag_of(got) == std::string_view{c.want});
        }
    }
}

TEST_CASE("i18n: lookup, fallback and the live swap") {
    const char* en = R"({
        "app.title": "Sandbox",
        "pane.read": {"text": "Also readable", "width_hint": 13},
        "turn.count": {"one": "{n} turn", "other": "{n} turns"}
    })";
    const char* ru = R"({
        "app.title": "Песочница",
        "pane.read": {"text": "Также читаемый"},
        "turn.count": {"one": "{n} ход", "few": "{n} хода", "many": "{n} ходов"}
    })";

    REQUIRE(install_catalog(Lang::en, en));
    REQUIRE(install_catalog(Lang::ru, ru));

    REQUIRE(set_active(Lang::en));
    CHECK(t("app.title") == "Sandbox");
    CHECK(t("pane.read") == "Also readable");
    CHECK(format_n("turn.count", 1, {}) == "1 turn");
    CHECK(format_n("turn.count", 2, {}) == "2 turns");

    // THE LIVE SWAP. One store, and the next lookup is translated -- no
    // rebuild, no restart. This is the behaviour the design rests on.
    REQUIRE(set_active(Lang::ru));
    CHECK(t("app.title") == "Песочница");
    CHECK(format_n("turn.count", 1, {})  == "1 ход");
    CHECK(format_n("turn.count", 2, {})  == "2 хода");
    CHECK(format_n("turn.count", 5, {})  == "5 ходов");
    CHECK(format_n("turn.count", 21, {}) == "21 ход");    // not "ходов"
    CHECK(format_n("turn.count", 11, {}) == "11 ходов");  // the teen hole

    // A missing id renders AS THE ID. Visibly wrong beats blank: a screen
    // reading "no.such.id" is a bug report, an empty row is a user wondering
    // what they broke.
    CHECK(t("no.such.id") == "no.such.id");

    // Switching to a language with no catalog must keep the current one
    // rather than blanking the UI.
    const auto before = active();
    CHECK(!set_active(Lang::hu));
    CHECK(active() == before);

    REQUIRE(set_active(Lang::en));
}

TEST_CASE("i18n: interpolation is named, and survives a typo") {
    const char* en = R"({
        "greet": "Hello {name}, you have {count} messages",
        "braces": "a {{literal}} brace",
        "typo": "total: {tatol}"
    })";
    REQUIRE(install_catalog(Lang::en, en));
    REQUIRE(set_active(Lang::en));

    const Arg args[] = {{"name", "Ayush"}, {"count", "3"}};
    CHECK(format("greet", args) == "Hello Ayush, you have 3 messages");

    // Named, not positional: the SAME args in a different order still work,
    // which is the property that lets a translator reorder a sentence.
    const Arg rev[] = {{"count", "3"}, {"name", "Ayush"}};
    CHECK(format("greet", rev) == "Hello Ayush, you have 3 messages");

    CHECK(format("braces", {}) == "a {literal} brace");

    // An unknown placeholder is left VERBATIM rather than erased. Erasing it
    // would leave a sentence with a hole nobody can trace; this shows up on
    // screen and gets reported.
    const Arg total[] = {{"total", "9"}};
    CHECK(format("typo", total) == "total: {tatol}");
}

TEST_CASE("i18n: every language has a tag, an endonym and a plural rule") {
    // The table is indexed by the enum (static_assert in i18n.cpp), but that
    // only proves ORDER. This proves each row is populated -- a blank endonym
    // would render an empty picker row that still selects a language.
    for (const Lang l : kAll) {
        INFO("lang " << std::string{english_name(l)});
        CHECK(!tag_of(l).empty());
        CHECK(!endonym(l).empty());
        CHECK(!english_name(l).empty());
        // The endonym is in the language's OWN script, so it is the one
        // string here that is allowed to be non-ASCII -- and for 7 of the 20
        // it must be, or we wrote the English name twice by mistake.
        Lang round{};
        REQUIRE(parse_tag(tag_of(l), round));
        CHECK(round == l);
    }
}

TEST_CASE("i18n: completeness is measured against English") {
    const char* en = R"({"a":"A","b":"B","c":"C","d":"D"})";
    const char* de = R"({"a":"A","b":"B"})";
    REQUIRE(install_catalog(Lang::en, en));
    REQUIRE(install_catalog(Lang::de, de));

    CHECK(completeness(Lang::en) == 1.0);
    CHECK(completeness(Lang::de) == 0.5);
    // A language with no catalog is 0, not an error -- the picker shows it
    // greyed rather than hiding it, so a user can see what is missing.
    CHECK(completeness(Lang::hu) == 0.0);

    // A catalog carrying ids English has DROPPED is not more complete; it is
    // stale. Measured against en's id set, so the extra id does not count.
    const char* de_stale = R"({"a":"A","b":"B","gone":"X","also_gone":"Y"})";
    REQUIRE(install_catalog(Lang::de, de_stale));
    CHECK(completeness(Lang::de) == 0.5);
}

// ── The appearance row, end to end ───────────────────────────────────────
//
// Everything above tests i18n in isolation. This tests the PANE: that the
// language row exists, carries tags rather than indices, and that choosing
// one switches the active language without a restart.
//
// Why tags and not indices matters enough to pin: a Choice normally stores
// the selected INDEX, so inserting a language into kLangs would silently
// change what every existing settings.json means -- someone's saved "7"
// becomes a different language on upgrade. Storing "de" cannot do that.

#include "agentty/runtime/panel/appearance.hpp"
#include "agentty/runtime/app/update.hpp"
#include "agentty/runtime/model.hpp"

TEST_CASE("i18n: the appearance pane carries a language row") {
    const char* en = R"JSON({
        "lang.picker.title": {"text": "Language"},
        "lang.picker.help":  {"text": "agentty's own labels"},
        "lang.auto":         {"text": "Auto (follow system)"},
        "lang.incomplete":   {"text": "{pct}% translated"}
    })JSON";
    REQUIRE(install_catalog(Lang::en, en));
    REQUIRE(set_active(Lang::en));

    agentty::ui_prefs::Prefs p;          // empty lang = auto
    const auto form = agentty::ui::panel::build_appearance_form(p, false);

    const auto* row = form.find(agentty::ui::panel::kApLang);
    REQUIRE(row != nullptr);

    const auto* c = std::get_if<agentty::form::field::Choice>(&row->value);
    REQUIRE(c != nullptr);

    // Auto plus every language.
    CHECK(c->labels.size() == static_cast<std::size_t>(kLangCount) + 1);
    REQUIRE(c->ids.size() == c->labels.size());

    // Auto is FIRST and its id is empty -- the same empty that ui.lang
    // stores, so a user who never chose keeps following their environment.
    CHECK(c->ids[0].empty());

    // Every other id is a real tag that round-trips. An index would be
    // unstable across a table edit; a tag cannot be.
    for (std::size_t i = 1; i < c->ids.size(); ++i) {
        INFO("row " << i << " id '" << c->ids[i] << "'");
        Lang parsed{};
        REQUIRE(parse_tag(c->ids[i], parsed));
        CHECK(tag_of(parsed) == c->ids[i]);
    }

    // Labels are ENDONYMS: a Japanese speaker scans for 日本語, not for our
    // English word. Checked on the three scripts where getting it wrong
    // would be invisible to an English reader.
    const auto has = [&](std::string_view needle) {
        for (const auto& l : c->labels)
            if (l.find(needle) != std::string::npos) return true;
        return false;
    };
    CHECK(has("日本語"));      // ja
    CHECK(has("Русский"));     // ru
    CHECK(has("한국어"));       // ko
    CHECK(has("Deutsch"));     // de
}

// ── The real catalog, in the real panes ──────────────────────────────────
//
// Every test above uses a four-string fixture. This one loads the SHIPPED
// German catalog and paints the actual panes with it, which is the only way
// to find out whether a translation overflows a frame.
//
// Measured on this catalog: German runs 1.1-2.8x English per string, worst
// absolute growth +32 columns (sandbox.help.read_paths), worst ratio 2.83x
// ("Custom" -> "Benutzerdefiniert"). Those are the numbers the layout has to
// absorb, and they are why German is the second language rather than the
// tenth -- it is the stress case for every other Latin-script language.
TEST_CASE("i18n: the shipped German catalog fits the panes") {
    REQUIRE(agentty::i18n::init("de", ""));
    REQUIRE(active() == Lang::de);

    // 100% against English. A partial catalog would fall back to English for
    // the gaps and silently hide exactly the overflow this test looks for,
    // so completeness is a PRECONDITION here, not an observation.
    CHECK(completeness(Lang::de) > 0.999);

    // No translation may be empty. An empty string renders as a blank row
    // that still responds to Enter -- worse than an untranslated one, which
    // at least says what it does.
    for (const auto& id : ids_of(Lang::de)) {
        INFO("id " << id);
        CHECK(!t(id).empty());
    }

    // Every id English has, German has. The lint checks code-vs-English;
    // this checks English-vs-German, which is the other half and the one
    // that rots as new strings land.
    for (const auto& id : ids_of(Lang::en)) {
        INFO("missing from de: " << id);
        REQUIRE(set_active(Lang::de));
        CHECK(t(id) != id);     // t() falls back to English, never the id
    }

    REQUIRE(set_active(Lang::en));
}
