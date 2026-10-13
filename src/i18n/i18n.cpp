// agentty::i18n — implementation.
//
// See i18n.hpp for the design and plural.hpp for the CLDR rules. This file
// is the storage and the lookup, and it has three jobs worth naming:
//
//   1. hold catalogs immutably, so t() can hand out string_views into them
//   2. swap the active one atomically, so a language change is a pointer
//      store and the next frame is translated (docs/LATE_BINDING.md)
//   3. parse a catalog without a JSON dependency on the hot path
//
// On (3): the catalogs are our own files, embedded in the binary, shaped by
// the schema in docs/design/i18n.md. They are parsed ONCE at install time.
// nlohmann is already a dependency and is used here rather than hand-rolling
// a parser -- the thing to avoid was a parser on the RENDER path, and there
// is none: after install_catalog the structure is a flat hash map.

#include "agentty/i18n/i18n.hpp"
#include "agentty/i18n/plural.hpp"

#include <maya/runtime.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <unordered_map>

namespace agentty::i18n {

namespace {

// ── The language table ───────────────────────────────────────────────────
//
// One row per language, in the order of the enum, with a static_assert that
// the two cannot drift. Endonyms are the name in the language's OWN script,
// because a user scanning the picker for their language is looking for the
// word they would use -- a Japanese speaker finds 日本語, not "Japanese".
struct Row {
    Lang             lang;
    std::string_view tag;
    std::string_view endonym;
    std::string_view english;
};

constexpr std::array<Row, kLangCount> kLangs{{
    {Lang::en,    "en",    "English",     "English"},
    {Lang::zh_CN, "zh-CN", "简体中文",      "Simplified Chinese"},
    {Lang::es,    "es",    "Español",     "Spanish"},
    {Lang::pt_BR, "pt-BR", "Português (Brasil)", "Portuguese (Brazil)"},
    {Lang::ja,    "ja",    "日本語",        "Japanese"},
    {Lang::de,    "de",    "Deutsch",     "German"},
    {Lang::fr,    "fr",    "Français",    "French"},
    {Lang::ko,    "ko",    "한국어",        "Korean"},
    {Lang::it,    "it",    "Italiano",    "Italian"},
    {Lang::ru,    "ru",    "Русский",     "Russian"},
    {Lang::tr,    "tr",    "Türkçe",      "Turkish"},
    {Lang::id,    "id",    "Bahasa Indonesia", "Indonesian"},
    {Lang::pl,    "pl",    "Polski",      "Polish"},
    {Lang::nl,    "nl",    "Nederlands",  "Dutch"},
    {Lang::uk,    "uk",    "Українська",  "Ukrainian"},
    {Lang::sv,    "sv",    "Svenska",     "Swedish"},
    {Lang::cs,    "cs",    "Čeština",     "Czech"},
    {Lang::da,    "da",    "Dansk",       "Danish"},
    {Lang::no,    "no",    "Norsk",       "Norwegian"},
    {Lang::hu,    "hu",    "Magyar",      "Hungarian"},
}};

// The table is indexed by the enum value, so a row in the wrong place would
// give every language after it the wrong name. Cheap to prove, impossible to
// notice by eye.
consteval bool table_is_ordered() {
    for (std::size_t i = 0; i < kLangs.size(); ++i)
        if (static_cast<std::size_t>(kLangs[i].lang) != i) return false;
    return true;
}
static_assert(table_is_ordered(),
              "kLangs must be indexed by Lang -- row N describes Lang(N)");

// ── Storage ──────────────────────────────────────────────────────────────
//
// A catalog is immutable once built. `Entry` holds either a single string or
// the plural forms; both are owned std::strings so a string_view into them
// stays valid for the catalog's life.
struct Entry {
    std::string                                     text;    // non-plural
    std::array<std::string, 4>                      forms{};  // by Plural
    bool                                            plural = false;
};

// Heterogeneous lookup, so t("some.id") does not allocate a std::string to
// probe the map. This is the difference between 19 ns and an allocation per
// call, and t() is called a few hundred times a frame.
struct Hash {
    using is_transparent = void;
    [[nodiscard]] std::size_t operator()(std::string_view s) const noexcept {
        return std::hash<std::string_view>{}(s);
    }
};
struct Eq {
    using is_transparent = void;
    [[nodiscard]] bool operator()(std::string_view a,
                                  std::string_view b) const noexcept {
        return a == b;
    }
};

using Table = std::unordered_map<std::string, Entry, Hash, Eq>;

struct Catalog {
    Lang  lang = Lang::en;
    Table table;
};

using CatalogPtr = std::shared_ptr<const Catalog>;

// Every installed catalog, by language. Written at startup and by the debug
// hot reload; read by set_active and completeness.
//
// A plain array of shared_ptr rather than a map: twenty slots, indexed by the
// enum, no lookup and no allocation.
std::array<maya::published<const Catalog>, kLangCount>& store() {
    static std::array<maya::published<const Catalog>, kLangCount> s;
    return s;
}

// The ACTIVE catalog. One published slot -- this is the whole live-switch
// mechanism. The outgoing catalog stays alive through the shared_ptr until the
// last reader drops it, which is what makes a string_view handed out during
// a frame safe even if the language changes mid-frame.
maya::published<const Catalog>& active_catalog() {
    static maya::published<const Catalog> c;
    return c;
}


// Lower-case ASCII only. Deliberately NOT std::tolower with a locale: under
// a Turkish locale that maps 'I' to dotless 'ı', and a tag comparison that
// changes meaning with the user's locale is the classic i18n self-own.
[[nodiscard]] char ascii_lower(char c) noexcept {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

[[nodiscard]] std::string normalize_tag(std::string_view in) {
    std::string out;
    out.reserve(in.size());
    for (const char c : in) {
        // "de_DE.UTF-8" and "de_DE@euro" both mean de. Stop at the encoding
        // or modifier; keep the region, which parse_tag may want.
        if (c == '.' || c == '@') break;
        out.push_back(c == '_' ? '-' : ascii_lower(c));
    }
    return out;
}

// An id the ACTIVE catalog does not carry: try English before giving up.
//
// THE CASE THIS EXISTS FOR, and a render test caught it rather than a reader:
// every real translation is partial for a while. A German catalog at 60%
// used to paint `sandbox.ports` as a row label -- the raw id -- next to
// perfectly good German. That is worse than English in every way: it is not
// a word in any language, it is wider than the label it replaced, and it
// tells the user nothing except that something is broken.
//
// English is the one catalog guaranteed installed (i18n::init loads it
// whatever the choice), and it is the language the ids are written in
// anyway. So the ladder is: active -> English -> the id. The last rung is
// reachable only when the English catalog itself is missing an entry, which
// the lint makes a build failure.
[[nodiscard]] std::string_view fallback(std::string_view id) noexcept;

}  // namespace

std::string_view tag_of(Lang l) noexcept {
    return kLangs[static_cast<std::size_t>(l)].tag;
}

std::string_view endonym(Lang l) noexcept {
    return kLangs[static_cast<std::size_t>(l)].endonym;
}

std::string_view english_name(Lang l) noexcept {
    return kLangs[static_cast<std::size_t>(l)].english;
}

bool parse_tag(std::string_view tag, Lang& out) noexcept {
    const std::string norm = normalize_tag(tag);
    if (norm.empty()) return false;

    // Exact match on the full tag first ("pt-br" -> pt_BR), so a regional
    // language we DO carry is never shadowed by its base.
    for (const auto& r : kLangs) {
        std::string lower;
        lower.reserve(r.tag.size());
        for (const char c : r.tag) lower.push_back(ascii_lower(c));
        if (lower == norm) { out = r.lang; return true; }
    }

    // Then the base subtag. "de-AT" -> de, "es-MX" -> es.
    const auto dash = norm.find('-');
    const std::string base = norm.substr(0, dash);

    // zh is the exception that earns its own branch. We carry Simplified
    // only, and Traditional is not a dialect of it -- serving zh-CN to a
    // zh-TW user is worse than English, because it LOOKS right and is not.
    // Same for zh-HK and zh-MO, which use Traditional.
    if (base == "zh") {
        if (dash == std::string_view::npos) { out = Lang::zh_CN; return true; }
        const std::string region = norm.substr(dash + 1);
        if (region == "cn" || region == "hans" || region == "sg") {
            out = Lang::zh_CN;
            return true;
        }
        return false;   // tw, hk, mo, hant -> fall through to English
    }

    // pt-PT -> pt-BR. Not identical, but close enough that a Portuguese
    // speaker is far better served than by English. The asymmetry with zh is
    // the point: mutual intelligibility is a per-language fact, not a rule.
    if (base == "pt") { out = Lang::pt_BR; return true; }

    // nb/nn both mean Norwegian for our purposes; CLDR splits them, we do not.
    if (base == "nb" || base == "nn") { out = Lang::no; return true; }

    for (const auto& r : kLangs) {
        const auto rdash = r.tag.find('-');
        std::string rbase{r.tag.substr(0, rdash)};
        for (auto& c : rbase) c = ascii_lower(c);
        if (rbase == base) { out = r.lang; return true; }
    }
    return false;
}

// The active catalog carries its language, so the two can never disagree.
Lang active() noexcept {
    const auto cat = active_catalog().current();
    return cat ? cat->lang : Lang::en;
}

namespace {
std::string_view fallback(std::string_view id) noexcept {
    const auto en = store()[static_cast<std::size_t>(Lang::en)].current();
    if (!en) return id;
    const auto it = en->table.find(id);
    if (it == en->table.end()) return id;
    if (it->second.plural)
        return it->second.forms[static_cast<std::size_t>(Plural::Other)];
    return it->second.text;
}
}  // namespace

bool set_active(Lang l) {
    auto cat = store()[static_cast<std::size_t>(l)].current();
    if (!cat) return false;     // no catalog compiled in: keep what we have
    active_catalog().publish(cat);
    return true;
}

std::string_view t(std::string_view id) noexcept {
    const auto cat = active_catalog().current();
    if (!cat) return id;        // before load(): the id, never blank
    const auto it = cat->table.find(id);
    if (it == cat->table.end()) return fallback(id);
    // A plural entry read through t() has no count to select on. `other` is
    // the one category every rule can produce, so it is the honest fallback.
    if (it->second.plural)
        return it->second.forms[static_cast<std::size_t>(Plural::Other)];
    return it->second.text;
}

std::string_view tn(std::string_view id, long long n) noexcept {
    const auto cat = active_catalog().current();
    if (!cat) return id;
    const auto it = cat->table.find(id);
    if (it == cat->table.end()) return fallback(id);
    if (!it->second.plural) return it->second.text;

    const Plural p = select(cat->lang, n);
    const auto& picked = it->second.forms[static_cast<std::size_t>(p)];
    // A category the catalog did not supply. The lint makes this
    // unreachable, but falling back to `other` beats returning empty -- and
    // `other` is populated for every rule shape by construction.
    if (picked.empty())
        return it->second.forms[static_cast<std::size_t>(Plural::Other)];
    return picked;
}

namespace {

// Substitute {name} placeholders.
//
// Unknown names are left VERBATIM rather than erased: a typo in a catalog
// shows on screen as "{tatol}", which is a bug report. Erasing it would
// produce a sentence with a hole in it that nobody can trace back.
//
// "{{" and "}}" are literal braces, so a string can contain one. BOTH halves
// are needed and the first version only had the opener: "a {{literal}} brace"
// then produced "a {literal}} brace", because the closer fell through to the
// verbatim copy below. Caught by the test, which is the only reason it is
// not shipping -- a stray brace in one string is exactly the kind of thing
// that survives review.
[[nodiscard]] std::string interpolate(std::string_view tmpl,
                                      std::span<const Arg> args) {
    std::string out;
    out.reserve(tmpl.size() + 16);
    for (std::size_t i = 0; i < tmpl.size(); ++i) {
        if (tmpl[i] == '}') {
            // "}}" -> "}". A lone '}' is copied as-is: it is almost
            // certainly a typo, and showing it beats swallowing it.
            if (i + 1 < tmpl.size() && tmpl[i + 1] == '}') ++i;
            out.push_back('}');
            continue;
        }
        if (tmpl[i] != '{') { out.push_back(tmpl[i]); continue; }
        if (i + 1 < tmpl.size() && tmpl[i + 1] == '{') { out.push_back('{'); ++i; continue; }

        const auto close = tmpl.find('}', i + 1);
        if (close == std::string_view::npos) { out.push_back('{'); continue; }

        const auto name = tmpl.substr(i + 1, close - i - 1);
        const auto hit = std::ranges::find_if(
            args, [&](const Arg& a) { return a.name == name; });
        if (hit != args.end()) out += hit->value;
        else { out.push_back('{'); out += name; out.push_back('}'); }
        i = close;
    }
    return out;
}

}  // namespace

std::string format(std::string_view id, std::span<const Arg> args) {
    return interpolate(t(id), args);
}

std::string format_n(std::string_view id, long long n, std::span<const Arg> args) {
    // `n` is always available as {n} without the caller restating it: every
    // plural string needs the number, and making each call site pass it is a
    // step that is only ever forgotten.
    std::vector<Arg> all;
    all.reserve(args.size() + 1);
    all.push_back(Arg{"n", std::to_string(n)});
    all.insert(all.end(), args.begin(), args.end());
    return interpolate(tn(id, n), all);
}

bool install_catalog(Lang l, std::string_view json_text) {
    nlohmann::json j;
    try {
        j = nlohmann::json::parse(json_text);
    } catch (...) {
        return false;
    }
    if (!j.is_object()) return false;

    auto cat = std::make_shared<Catalog>();
    cat->lang = l;

    for (const auto& [key, val] : j.items()) {
        if (key.starts_with('_')) continue;   // _meta and friends

        Entry e;
        if (val.is_string()) {
            e.text = val.get<std::string>();
        } else if (val.is_object()) {
            // Plural entry. A string-valued "text" inside an object is the
            // non-plural form carrying a width_hint, which the lint reads and
            // the runtime does not.
            if (val.contains("text") && val["text"].is_string()) {
                e.text = val["text"].get<std::string>();
            } else {
                e.plural = true;
                const auto take = [&](const char* k, Plural p) {
                    if (val.contains(k) && val[k].is_string())
                        e.forms[static_cast<std::size_t>(p)] =
                            val[k].get<std::string>();
                };
                take("one", Plural::One);
                take("few", Plural::Few);
                take("many", Plural::Many);
                take("other", Plural::Other);
                // `other` is what every fallback lands on, so a plural entry
                // without it is unusable. Reject the whole catalog rather
                // than install one that can return empty.
                if (e.forms[static_cast<std::size_t>(Plural::Other)].empty()) {
                    // Rule shapes without `other` (ru/uk/pl) legitimately
                    // omit it; seed from `many` so the fallback path has
                    // something true to return.
                    const auto& many = e.forms[static_cast<std::size_t>(Plural::Many)];
                    if (many.empty()) return false;
                    e.forms[static_cast<std::size_t>(Plural::Other)] = many;
                }
            }
        } else {
            continue;   // a number or null in a catalog is a typo, not data
        }
        cat->table.emplace(key, std::move(e));
    }

    store()[static_cast<std::size_t>(l)].publish(cat);
    // Installing the ACTIVE language republishes it, so a hot reload in a
    // debug build takes effect without a second call.
    if (active() == l) active_catalog().publish(cat);
    return true;
}

std::vector<std::string> ids_of(Lang l) {
    const auto cat = store()[static_cast<std::size_t>(l)].current();
    if (!cat) return {};
    std::vector<std::string> out;
    out.reserve(cat->table.size());
    for (const auto& [k, _] : cat->table) out.push_back(k);
    std::ranges::sort(out);
    return out;
}

double completeness(Lang l) {
    const auto en = store()[static_cast<std::size_t>(Lang::en)].current();
    if (!en || en->table.empty()) return 0.0;
    if (l == Lang::en) return 1.0;

    const auto cat = store()[static_cast<std::size_t>(l)].current();
    if (!cat) return 0.0;

    // Against en's id set, not against the catalog's own size: a translation
    // carrying ids en has dropped is not more complete, it is stale.
    std::size_t have = 0;
    for (const auto& [k, _] : en->table)
        if (cat->table.contains(k)) ++have;
    return static_cast<double>(have) / static_cast<double>(en->table.size());
}

}  // namespace agentty::i18n
