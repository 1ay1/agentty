#pragma once
// agentty::i18n — the translation lookup.
//
// Design: docs/design/i18n.md. The short version of the four decisions, so a
// reader here does not have to go and get the long one:
//
//   NO GETTEXT. It needs libintl (we ship one static binary, including musl
//   and Termux), its plural rules are a C expression evaluated at runtime out
//   of a file on disk, and msgid-as-English means an English copy-edit
//   silently invalidates every translation. We key on IDS.
//
//   NO ICU. Measured: 38 MB, of which 33 MB is libicudata carrying ~600
//   locales to serve the 20 we ship. The one thing it would genuinely buy --
//   plural rules -- collapses to five rule shapes across our language set
//   (plural.hpp), and the genuinely hard problem, double-width CJK in a cell
//   grid, is already solved correctly by maya::string_width.
//
//   NOT EVERYTHING IS TRANSLATED. ~5000 string literals exist in the tree and
//   most are for the MODEL, not the user: tool descriptions, the sandbox
//   denial note, error detail. Those are part of a contract with a provider,
//   or exist specifically to change model behaviour, and the model follows
//   English best. The rule is: IF IT CROSSES THE WIRE, IT STAYS ENGLISH.
//
//   LIVE, NOT SEALED. The sandbox is set-once because a boundary that moves
//   under a running process is unauditable. A language is not a boundary.
//
// ── Why this is a pointer swap and not a rebuild ─────────────────────────
//
// t() reads an immutable catalog through an atomic pointer and returns a
// string_view INTO it. That makes translations late-bound exactly the way
// colours are (docs/LATE_BINDING.md): nothing already built baked the English
// in, because the view calls t() on every frame it paints. Switching language
// stores one pointer and the next frame is translated.
//
// Measured on this shape:
//
//     switching language   52 ns    one atomic store
//     one t() lookup       19 ns    zero allocation
//     a frame (~200)      ~4 us     against a 16 ms budget
//
// which is why the render path may call t() freely rather than caching
// translated strings somewhere that would then need invalidating.
//
// LIFETIME. The returned view is valid as long as the catalog that produced
// it lives. Catalogs are never mutated and never freed while a frame might
// hold a view (the swap keeps the outgoing catalog alive through the
// shared_ptr until the last reader drops it), so a view captured during a
// frame stays valid for that frame. Do not store one across frames -- hold
// the id instead, which is a string literal and outlives everything.

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace agentty::i18n {

// ── The twenty ───────────────────────────────────────────────────────────
//
// A closed enum rather than free-form tags, because every language we claim
// needs a plural rule, a name in its own script, and a catalog -- and all
// three are compile-time facts. A new language is a deliberate edit here, not
// a file someone drops in a directory.
//
// The list stops where it does for a reason: Arabic, Hebrew, Persian, Thai
// and the Indic scripts need bidi or complex shaping that a cell-grid TUI
// cannot do correctly. Adding one is a real project, not another JSON file.
enum class Lang : std::uint8_t {
    en, zh_CN, es, pt_BR, ja, de, fr, ko, it, ru,
    tr, id, pl, nl, uk, sv, cs, da, no, hu,
};

inline constexpr int kLangCount = 20;

// BCP-47 tag, as it appears in settings.json and in --lang.
[[nodiscard]] std::string_view tag_of(Lang l) noexcept;

// Parse a tag. Case-insensitive, and tolerant of the shapes a real
// environment produces: "de_DE.UTF-8", "pt-BR", "zh_CN", "EN".
//
// Region fallback is deliberate and asymmetric:
//   pt-PT -> pt-BR   (close enough that a Portuguese speaker is served)
//   zh-TW -> NOT zh-CN. Traditional is not Simplified, and silently serving
//                       one for the other is worse than English.
[[nodiscard]] bool parse_tag(std::string_view tag, Lang& out) noexcept;

// The language's own name, in its own script ("Deutsch", "日本語"). What the
// picker shows: a user looking for their language is looking for the word
// they would use, not for our English name for it.
[[nodiscard]] std::string_view endonym(Lang l) noexcept;

// English name, for logs and for the lint's error messages.
[[nodiscard]] std::string_view english_name(Lang l) noexcept;

// ── Lookup ───────────────────────────────────────────────────────────────

// The active language. Defaults to en before load() runs, so a t() call on
// any startup path before the catalog lands returns English rather than an
// id -- startup order must not be able to produce visible garbage.
[[nodiscard]] Lang active() noexcept;

// Swap the active catalog. Returns false when the language has no catalog
// compiled in, leaving the previous one active -- a missing translation must
// never blank the UI.
bool set_active(Lang l);

// One translated string.
//
// A missing id returns THE ID ITSELF. Visibly wrong beats blank: a screen
// reading "sandbox.row.read_paths.label" is a bug report, an empty row is a
// user wondering if they broke something. The lint makes it unreachable in a
// shipped build (every t() id must exist in en.json), so this is the
// behaviour of a build that failed its own gate, not a design we rely on.
[[nodiscard]] std::string_view t(std::string_view id) noexcept;

// A translated string with a count, picking the CLDR plural category for the
// active language. See plural.hpp for why this cannot be a suffix rule.
[[nodiscard]] std::string_view tn(std::string_view id, long long n) noexcept;

// ── Interpolation ────────────────────────────────────────────────────────
//
// NAMED placeholders only: "{n} of {total}". Positional args reorder across
// languages -- German puts the verb last, Japanese puts the object first --
// and a translator cannot be expected to count. A name survives reordering.
//
// Unknown placeholders are left verbatim rather than erased, so a typo in a
// catalog shows up on screen as "{tatol}" instead of silently vanishing.
struct Arg {
    std::string_view name;
    std::string      value;
};

[[nodiscard]] std::string format(std::string_view id, std::span<const Arg> args);
[[nodiscard]] std::string format_n(std::string_view id, long long n,
                                   std::span<const Arg> args);

// ── Catalog loading ──────────────────────────────────────────────────────

// Install a catalog from JSON text. Used by the embedded tables at startup
// and by the debug-only hot reload from $AGENTTY_HOME/i18n/.
//
// Returns false (and changes nothing) on malformed input: a half-applied
// catalog would be a UI in two languages.
bool install_catalog(Lang l, std::string_view json_text);

// Every id the active catalog knows. For the lint and for tests.
[[nodiscard]] std::vector<std::string> ids_of(Lang l);

// How much of `l` is translated, 0..1, against en as the denominator. The
// picker shows this so a user choosing a 60%-complete language knows what
// they are getting rather than discovering it a screen at a time.
[[nodiscard]] double completeness(Lang l);

}  // namespace agentty::i18n
