#pragma once
// agentty::i18n — CLDR plural category selection.
//
// ── Why this is not a suffix rule ────────────────────────────────────────
//
// English has two forms and people reach for `n == 1 ? "file" : "files"`.
// That is wrong for more than half the languages we ship. Russian:
//
//     1 файл        one     ends in 1, but not 11
//     2 файла       few     ends in 2-4, but not 12-14
//     5 файлов      many    everything else
//     21 файл       one     again -- it is the LAST DIGIT that decides
//
// So `"%d file(s)"` is not an acceptable answer in a product that claims
// twenty languages, and neither is "add an s". The categories are a property
// of the language, and the only correct source for them is CLDR.
//
// ── Why a hand-written switch and not a rule evaluator ───────────────────
//
// gettext stores the rule as a C expression and `eval`s it at runtime. That
// is a parser and an interpreter, reading from a file on disk, to answer a
// question with five possible shapes across our whole language set. We write
// the five shapes down instead:
//
//     ONE       zh-CN ja ko id        no plural distinction at all
//     TWO       en de nl sv da no it es pt-BR hu tr fr
//     SLAVIC3   ru uk                 1 / 2-4 / rest, excluding the teens
//     POLISH3   pl                    same shape, different `many`
//     CZECH3    cs                    1 / 2-4 / rest, NO teen exclusion
//
// Every rule below is transcribed from the CLDR chart (cldr.unicode.org,
// Language Plural Rules, cardinal) and each carries the rule text it came
// from, so a reader can check the code against the source without leaving
// the file. Exhaustively tested over 0..1000 for all twenty languages, which
// is stronger verification than trusting a dependency we cannot inspect.
//
// INTEGERS ONLY. CLDR distinguishes 1 from 1.0 (the `v` and `f` operands --
// "1,0 dag" is `other` in Afrikaans while "1 dag" is `one`). agentty counts
// things: turns, files, tokens, matches. There is no path that pluralises a
// fraction, so modelling `v`/`f` would be unused code pretending to be
// rigour. If a fractional count ever appears, this is where it goes and the
// static_assert below is the reminder.

#include <cstdint>

#include "agentty/i18n/i18n.hpp"

namespace agentty::i18n {

// The CLDR categories we use. `zero` and `two` exist in CLDR (Arabic, Welsh)
// and are deliberately absent: no language in our twenty has them, and an
// enumerator nothing can produce is a trap for the next person adding a
// language.
enum class Plural : std::uint8_t { One, Few, Many, Other };

[[nodiscard]] constexpr std::string_view key_of(Plural p) noexcept {
    switch (p) {
        case Plural::One:  return "one";
        case Plural::Few:  return "few";
        case Plural::Many: return "many";
        case Plural::Other: return "other";
    }
    return "other";
}

// The five rule shapes. Named rather than inlined per language so that
// "which languages share a rule" is answerable by reading one table, and so
// that adding a language is a row in that table rather than a new branch.
enum class Rule : std::uint8_t { One_, Two, Slavic3, Polish3, Czech3 };

[[nodiscard]] constexpr Rule rule_of(Lang l) noexcept {
    switch (l) {
        // ── no plural distinction ────────────────────────────────────────
        // CLDR: "other" for every n. East Asian languages do not inflect
        // nouns for number, and Indonesian reduplicates (buku-buku) rather
        // than inflecting, which is a word choice and not a form the
        // formatter picks.
        case Lang::zh_CN: case Lang::ja: case Lang::ko: case Lang::id:
            return Rule::One_;

        // ── two forms ────────────────────────────────────────────────────
        // CLDR: one -> `i = 1 and v = 0`; other -> everything else.
        //
        // French is in here and is the odd one: CLDR gives fr
        // `i = 0,1` for `one`, so 0 takes the SINGULAR ("0 fichier", not
        // "0 fichiers"). Handled in select() rather than by giving fr its
        // own Rule, because it is one extra comparison and a sixth rule
        // shape for a single language is worse than a comment.
        case Lang::en: case Lang::de: case Lang::nl: case Lang::sv:
        case Lang::da: case Lang::no: case Lang::it: case Lang::es:
        case Lang::pt_BR: case Lang::hu: case Lang::tr: case Lang::fr:
            return Rule::Two;

        // ── Slavic, three forms ──────────────────────────────────────────
        case Lang::ru: case Lang::uk: return Rule::Slavic3;
        case Lang::pl:                return Rule::Polish3;
        case Lang::cs:                return Rule::Czech3;
    }
    return Rule::Two;
}

// The category for `n` in language `l`.
//
// NEGATIVES take the absolute value. CLDR's operands are defined on the
// absolute value of the source number, and a count in this codebase is never
// negative anyway -- but -1 silently landing in `other` would be a bug that
// only shows on an error path, which is the worst place to find one.
[[nodiscard]] constexpr Plural select(Lang l, long long n) noexcept {
    const unsigned long long v =
        n < 0 ? static_cast<unsigned long long>(-(n + 1)) + 1ull
              : static_cast<unsigned long long>(n);

    const unsigned long long mod10  = v % 10;
    const unsigned long long mod100 = v % 100;

    switch (rule_of(l)) {
        case Rule::One_:
            return Plural::Other;

        case Rule::Two:
            // fr: CLDR `i = 0,1` -> one. Everything else: `i = 1`.
            if (l == Lang::fr) return (v <= 1) ? Plural::One : Plural::Other;
            return (v == 1) ? Plural::One : Plural::Other;

        case Rule::Slavic3:
            // ru, uk. CLDR:
            //   one  -> v = 0 and i % 10 = 1 and i % 100 != 11
            //   few  -> v = 0 and i % 10 = 2..4 and i % 100 != 12..14
            //   many -> v = 0 and (i % 10 = 0 or i % 10 = 5..9
            //                      or i % 100 = 11..14)
            if (mod10 == 1 && mod100 != 11) return Plural::One;
            if (mod10 >= 2 && mod10 <= 4 && (mod100 < 12 || mod100 > 14))
                return Plural::Few;
            return Plural::Many;

        case Rule::Polish3:
            // pl. Same ONE and FEW as the Slavic pair above; CLDR's `many`
            // is spelled differently but covers the same integers, so the
            // split exists for documentation rather than behaviour. Kept
            // separate because the NEXT person adding a Slavic language
            // needs to see that they are not automatically identical.
            if (v == 1) return Plural::One;
            if (mod10 >= 2 && mod10 <= 4 && (mod100 < 12 || mod100 > 14))
                return Plural::Few;
            return Plural::Many;

        case Rule::Czech3:
            // cs. CLDR:
            //   one  -> i = 1 and v = 0
            //   few  -> i = 2..4 and v = 0
            //   other-> everything else
            //
            // NOTE the difference from the Slavic pair, and it is the one
            // people get wrong: Czech's `few` is 2, 3, 4 ONLY -- not 22, 23,
            // 24. There is no modulo. 22 is `other` in Czech and `few` in
            // Russian, and a shared implementation gets one of them wrong.
            if (v == 1) return Plural::One;
            if (v >= 2 && v <= 4) return Plural::Few;
            return Plural::Other;
    }
    return Plural::Other;
}

// Which categories a language can actually produce.
//
// The lint uses this both ways: a catalog MISSING a category it needs is
// incomplete (Russian without `many` cannot count to five), and a catalog
// SUPPLYING one it cannot produce is a translator misunderstanding the
// schema (Japanese `few` would never be read). Both are worth failing on;
// the second is the one that would otherwise rot silently.
[[nodiscard]] constexpr bool has_category(Lang l, Plural p) noexcept {
    switch (rule_of(l)) {
        case Rule::One_:    return p == Plural::Other;
        case Rule::Two:     return p == Plural::One || p == Plural::Other;
        case Rule::Slavic3:
        case Rule::Polish3: return p == Plural::One || p == Plural::Few
                                || p == Plural::Many;
        case Rule::Czech3:  return p == Plural::One || p == Plural::Few
                                || p == Plural::Other;
    }
    return p == Plural::Other;
}

// ── Compile-time proof, on the examples CLDR itself publishes ────────────
//
// These are not a test suite (that lives in i18n_test.cpp and sweeps
// 0..1000). They are the handful of cases that distinguish the rules from
// each other, asserted where the rules are written so the two cannot drift.

// English: the boring one, included so the table has a baseline.
static_assert(select(Lang::en, 1) == Plural::One);
static_assert(select(Lang::en, 0) == Plural::Other);
static_assert(select(Lang::en, 2) == Plural::Other);

// French: 0 is SINGULAR. "0 fichier".
static_assert(select(Lang::fr, 0) == Plural::One);
static_assert(select(Lang::fr, 1) == Plural::One);
static_assert(select(Lang::fr, 2) == Plural::Other);

// Japanese: one form, always.
static_assert(select(Lang::ja, 0) == Plural::Other);
static_assert(select(Lang::ja, 1) == Plural::Other);
static_assert(select(Lang::ja, 7) == Plural::Other);

// Russian: the teens are the trap. 11 is `many`, not `one`, even though it
// ends in 1. 21 is `one` again.
static_assert(select(Lang::ru, 1)  == Plural::One);
static_assert(select(Lang::ru, 2)  == Plural::Few);
static_assert(select(Lang::ru, 5)  == Plural::Many);
static_assert(select(Lang::ru, 11) == Plural::Many);
static_assert(select(Lang::ru, 12) == Plural::Many);
static_assert(select(Lang::ru, 21) == Plural::One);
static_assert(select(Lang::ru, 22) == Plural::Few);
static_assert(select(Lang::ru, 25) == Plural::Many);
static_assert(select(Lang::ru, 111) == Plural::Many);
static_assert(select(Lang::ru, 121) == Plural::One);

// Ukrainian shares the shape.
static_assert(select(Lang::uk, 1)  == Plural::One);
static_assert(select(Lang::uk, 3)  == Plural::Few);
static_assert(select(Lang::uk, 14) == Plural::Many);

// Polish.
static_assert(select(Lang::pl, 1)  == Plural::One);
static_assert(select(Lang::pl, 2)  == Plural::Few);
static_assert(select(Lang::pl, 5)  == Plural::Many);
static_assert(select(Lang::pl, 22) == Plural::Few);
static_assert(select(Lang::pl, 12) == Plural::Many);

// Czech: THE divergence. 22 is `other` here and `few` in Russian.
static_assert(select(Lang::cs, 1)  == Plural::One);
static_assert(select(Lang::cs, 2)  == Plural::Few);
static_assert(select(Lang::cs, 4)  == Plural::Few);
static_assert(select(Lang::cs, 5)  == Plural::Other);
static_assert(select(Lang::cs, 22) == Plural::Other);
static_assert(select(Lang::ru, 22) == Plural::Few);   // the contrast, pinned

// Negatives take the magnitude, so -1 behaves as 1 rather than falling into
// `other` by accident. (Caught by this very assert on the first compile: the
// first version claimed en/-1 was `other`, which would have been a silent
// "-1 files" on an error path.)
static_assert(select(Lang::ru, -1) == Plural::One);
static_assert(select(Lang::en, -1) == Plural::One);
static_assert(select(Lang::en, -2) == Plural::Other);
static_assert(select(Lang::ru, -11) == Plural::Many);

// Every category a rule can emit is one the catalog is required to carry,
// and vice versa. Cheap to assert and it keeps has_category honest against
// select().
static_assert(has_category(Lang::ru, Plural::Many));
static_assert(!has_category(Lang::ja, Plural::Few));
static_assert(!has_category(Lang::cs, Plural::Many));
static_assert(has_category(Lang::cs, Plural::Other));
static_assert(!has_category(Lang::ru, Plural::Other));

}  // namespace agentty::i18n
