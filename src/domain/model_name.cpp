// agentty::model_name — implementation of the display-name SSOT.
//
// Structure of this file:
//
//   1. `normalize_id_words` — the id → human-typography pass. This is the
//      ONLY string tokeniser here, and it is the one that used to live in
//      ui::pretty_model_label (moved down from the view layer, unchanged in
//      behaviour — its test suite still pins it). It handles snapshot dates,
//      :latest aliases, Ollama tags, acronym casing and version joins.
//
//   2. `decode` — delegates TAXONOMY to ModelCapabilities::from_id and uses
//      (1) only for the residual "we don't know this family" case and for
//      reconciling a server-supplied marketing name.
//
//   3. The projections.
//
// The division of labour is the whole design: from_id answers "which lane is
// this?", normalize answers "how do humans spell this token?", and nothing
// answers both.

#include "agentty/domain/model_name.hpp"

#include <algorithm>
#include <cctype>
#include <vector>

namespace agentty::model_name {

namespace {

// ── Character predicates (ASCII; model ids are ASCII by construction) ────
constexpr bool is_alpha(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}
constexpr bool is_upper(char c) { return c >= 'A' && c <= 'Z'; }
constexpr bool is_lower(char c) { return c >= 'a' && c <= 'z'; }
constexpr bool is_digit(char c) { return c >= '0' && c <= '9'; }

constexpr char to_lower(char c) {
    return is_upper(c) ? static_cast<char>(c - 'A' + 'a') : c;
}
constexpr char to_upper(char c) {
    return is_lower(c) ? static_cast<char>(c - 'a' + 'A') : c;
}

constexpr bool lower_eq(std::string_view w, std::string_view lit) {
    if (w.size() != lit.size()) return false;
    for (std::size_t i = 0; i < w.size(); ++i)
        if (to_lower(w[i]) != lit[i]) return false;
    return true;
}

constexpr bool all_digits(std::string_view w) {
    if (w.empty()) return false;
    for (char c : w)
        if (!is_digit(c)) return false;
    return true;
}

// An 8-digit "20YYMMDD" run — a release snapshot, not an identity.
constexpr bool is_snapshot_word(std::string_view w) {
    return w.size() == 8 && all_digits(w) && w.substr(0, 2) == "20";
}

// Alias/status tags a provider spells parenthetically in its display name:
// "GPT-5.3 Chat (latest)". Provenance, not identity.
constexpr bool is_paren_alias(std::string_view w) {
    if (w.size() < 2 || w.front() != '(' || w.back() != ')') return false;
    const std::string_view in = w.substr(1, w.size() - 2);
    return lower_eq(in, "latest") || lower_eq(in, "preview")
        || lower_eq(in, "deprecated") || lower_eq(in, "beta")
        || lower_eq(in, "exp") || lower_eq(in, "experimental");
}

// Pure quantization noise in an Ollama tag: changes fidelity, not identity,
// and reads like line noise in a picker row. `q4_K_M`, `Q8_0`, `fp16`.
constexpr bool is_quant(std::string_view p) {
    if (p.size() < 2) return false;
    if ((p.front() == 'q' || p.front() == 'Q') && is_digit(p[1])) return true;
    return p == "fp16" || p == "fp32" || p == "bf16";
}

// ── One word, in human typography ────────────────────────────────────────
//
// Rules, in precedence order:
//   • an existing all-caps acronym (≤4 chars) is kept verbatim — GPT, GLM, SQL
//   • a curated lowercase acronym is upper-cased — these read wrong title-cased
//   • a mixed-case brand name gets its brand spelling — ChatGPT, DeepSeek
//   • a DIGIT-LED word is a version/size run and stays lowercase — 4o, 8x7b, 70b
//   • the o-series convention: a lone 'o' before a digit is lowercase — o4-mini
//   • otherwise title-case the leading letter
void append_word(std::string& out, std::string_view w) {
    if (w.empty()) return;

    bool all_caps = true, has_alpha = false;
    for (char c : w)
        if (is_alpha(c)) {
            has_alpha = true;
            if (!is_upper(c)) all_caps = false;
        }
    if (has_alpha && all_caps && w.size() <= 4) {
        out.append(w);
        return;
    }

    for (std::string_view acr : {"gpt", "glm", "sql", "tts", "vl"})
        if (lower_eq(w, acr)) {
            for (char c : w) out.push_back(to_upper(c));
            return;
        }

    struct Brand { std::string_view key, spelling; };
    for (const Brand& b : {Brand{"chatgpt", "ChatGPT"},
                           Brand{"deepseek", "DeepSeek"},
                           Brand{"openai", "OpenAI"}})
        if (lower_eq(w, b.key)) {
            out.append(b.spelling);
            return;
        }

    if (is_digit(w.front())) {
        for (char c : w) out.push_back(to_lower(c));
        return;
    }

    if (w.size() >= 2 && (w[0] == 'o' || w[0] == 'O') && is_digit(w[1])) {
        out.push_back('o');
        out.append(w.substr(1));
        return;
    }

    bool cased = false;
    for (char c : w) {
        if (!cased && is_lower(c)) {
            out.push_back(to_upper(c));
            cased = true;
        } else {
            if (is_alpha(c)) cased = true;
            out.push_back(c);
        }
    }
}

// ── id → human typography ────────────────────────────────────────────────
//
// Handles the shapes that appear in the wild:
//   claude-3-5-haiku-20241022   snapshot word     → "Claude 3.5 Haiku"
//   gpt-4o-2024-08-06           4-2-2 date triple → "GPT 4o"
//   chatgpt-4o-latest           alias pointer     → "ChatGPT 4o"
//   qwen2.5-coder:7b            Ollama size tag   → "Qwen2.5 Coder 7b"
//   llama3:70b-instruct-q4_K_M  tag chain + quant → "Llama3 70b Instruct"
//   openrouter/anthropic/…      provider namespace stripped
//
// Note it does NOT strip [1m]/[2m]: decode() peels those first so the marker
// becomes a structured `annotation` instead of being silently discarded (the
// bug where a paid-for 1M window was invisible in the badge).
std::string normalize_id(std::string_view id) {
    // Provider namespace: keep the segment after the last '/'.
    if (const auto slash = id.find_last_of('/');
        slash != std::string_view::npos)
        id.remove_prefix(slash + 1);

    // Ollama `:tag` (size / variant / quant selector).
    std::string_view tag;
    if (const auto colon = id.find(':'); colon != std::string_view::npos) {
        tag = id.substr(colon + 1);
        id  = id.substr(0, colon);
    }
    // `:latest` carries no information — every pull defaults to it.
    if (lower_eq(tag, "latest")) tag = {};

    // Collect words FIRST so neighbour-aware rules (version joins, date
    // triples) can see the whole id instead of streaming blind.
    std::vector<std::string_view> words;
    std::size_t start = 0;
    for (std::size_t i = 0; i <= id.size(); ++i) {
        const bool boundary =
            (i == id.size() || id[i] == '-' || id[i] == '_' || id[i] == ' ');
        if (!boundary) continue;
        if (i > start) words.push_back(id.substr(start, i - start));
        start = i + 1;
    }

    // Drop a trailing release snapshot — provenance, not identity.
    if (!words.empty() && is_snapshot_word(words.back())) {
        words.pop_back();
    } else if (words.size() >= 4) {   // ≥4 so we never reduce an id to nothing
        const auto y = words[words.size() - 3];
        const auto m = words[words.size() - 2];
        const auto d = words[words.size() - 1];
        if (y.size() == 4 && all_digits(y) && y.substr(0, 2) == "20"
            && m.size() == 2 && all_digits(m)
            && d.size() == 2 && all_digits(d))
            words.resize(words.size() - 3);
    }
    // A trailing "latest" / "(preview)" is an alias pointer, not a name.
    if (words.size() > 1 && lower_eq(words.back(), "latest"))
        words.pop_back();
    if (words.size() > 1 && is_paren_alias(words.back()))
        words.pop_back();

    std::string out;
    out.reserve(id.size() + tag.size() + 1);
    bool prev_short_number = false;
    for (const std::string_view w : words) {
        // Version join: adjacent short pure-digit words are ONE dotted
        // version, not two numbers — `claude-sonnet-4-5` is Sonnet 4.5,
        // `claude-3-5-haiku` is 3.5. Size/quant words (9b, 8x7b) contain
        // letters so they never join; snapshots were dropped above.
        const bool short_number = all_digits(w) && w.size() <= 2;
        if (short_number && prev_short_number) {
            out.push_back('.');
            out.append(w);
        } else {
            if (!out.empty()) out.push_back(' ');
            append_word(out, w);
        }
        prev_short_number = short_number;
    }
    if (out.empty()) out.assign(id);   // pathological all-delimiter id

    // Ollama tag chain: keep what a human distinguishes models BY (size
    // `70b`, variant `instruct`/`coder`), drop quantization.
    if (!tag.empty()) {
        std::string cleaned;
        std::size_t p0 = 0;
        for (std::size_t i = 0; i <= tag.size(); ++i) {
            if (i != tag.size() && tag[i] != '-') continue;
            const std::string_view part = tag.substr(p0, i - p0);
            p0 = i + 1;
            if (part.empty() || is_quant(part)) continue;
            if (!cleaned.empty()) cleaned.push_back(' ');
            append_word(cleaned, part);
        }
        if (!cleaned.empty()) {
            out.push_back(' ');
            out.append(cleaned);
        }
    }
    return out;
}

// Alphanumeric-only, case-folded skeleton. Two strings with the same
// skeleton are the same model spelled differently ("GPT-4o" vs "gpt-4o"),
// carrying no marketing signal.
std::string skeleton(std::string_view s) {
    std::string k;
    k.reserve(s.size());
    for (char c : s)
        if (std::isalnum(static_cast<unsigned char>(c)))
            k.push_back(to_lower(c));
    return k;
}

// ── Input hardening ──────────────────────────────────────────────────────
//
// Model ids arrive from provider APIs and user config. NEITHER is trusted:
// a `/v1/models` response is attacker-controlled if the endpoint is (a
// custom host, a compromised aggregator, a typo-squatted proxy), and it
// flows straight to a terminal that interprets what it is handed.
//
// Two properties are enforced here, at the boundary, so no downstream
// projection or widget has to remember them:
//
//   1. NO CONTROL CHARACTERS reach the screen. An id containing
//      "\x1b[31m" would otherwise emit a LIVE escape sequence — recolouring
//      the UI, moving the cursor, or corrupting the display from a string
//      the user never typed. This is a terminal-injection bug, not a
//      cosmetic one, and it is why sanitising belongs in the decoder rather
//      than in each caller.
//
//   2. LENGTH IS BOUNDED. A 4 KB id is not a name; it is a denial of
//      service against every width-shed ladder downstream (the composer
//      footer, the status bar, the picker rows all measure what they are
//      given). Truncation is visible — an ellipsis, not a silent cut — so a
//      surprising label reads as "too long" rather than as "wrong".
//
// Both are deliberately dumb: drop bytes, cap length. Worse-is-better — a
// simple rule applied at one choke point beats a clever one applied in
// several places, because there is nothing to get subtly wrong and nothing
// to forget at a new call site.

// Longest label we will ever emit. Comfortably past the longest real model
// name ("Qwen2.5 Coder 32b Instruct" is 26) and far short of any width a
// shed ladder must cope with.
constexpr std::size_t kMaxNameLen = 48;

// Longest id we will even look at. Beyond this the input is not a model id
// by any reasonable reading, and parsing it wastes work proportional to an
// attacker's input.
constexpr std::size_t kMaxIdLen = 256;

// True for a byte that must never reach the terminal: C0 controls (which
// include ESC, CR, LF, TAB, BEL) and DEL. UTF-8 continuation bytes are all
// >= 0x80 and so pass through untouched — "Modèle" and "模型" survive.
constexpr bool is_control_byte(char c) {
    const auto u = static_cast<unsigned char>(c);
    return u < 0x20 || u == 0x7F;
}

// Drop control bytes and collapse the whitespace they leave behind.
std::string strip_controls(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s)
        if (!is_control_byte(c)) out.push_back(c);
    return out;
}

// Cap a finished label, marking the cut so truncation is legible rather
// than mysterious. Truncates on a UTF-8 boundary: cutting mid-sequence
// would emit a replacement glyph or, worse, desynchronise the renderer's
// width accounting.
std::string clamp_len(std::string s) {
    if (s.size() <= kMaxNameLen) return s;
    std::size_t cut = kMaxNameLen - 1;
    // Walk back off any UTF-8 continuation byte (10xxxxxx).
    while (cut > 0 && (static_cast<unsigned char>(s[cut]) & 0xC0) == 0x80)
        --cut;
    s.resize(cut);
    s.append("\xe2\x80\xa6");   // …
    return s;
}

// ── Vendor prefixes ──────────────────────────────────────────────────────
//
// A leading vendor token in the id (`claude-`, `anthropic.`, `openai/`) is
// NOT part of the model's name — it is provider/vendor provenance, and the
// provider is already rendered from the registry row by its own chip.
//
// This matters most for a family the table does NOT know yet. `claude-opus-4-8`
// decodes via the family table and renders "Opus 4.8", vendor-free. Without
// this strip, the day Anthropic ships `claude-quasar-6` it would render
// "Claude Quasar 6" — inconsistent typography appearing at exactly the moment
// a user is most likely to be looking at it. Stripping here means an unknown
// family degrades to the SAME shape as a known one.
//
// Only leading tokens are stripped, and only exact matches: `claude-opus` is
// prefixed, but a model genuinely named `gemma` keeps its name.
constexpr std::string_view kVendorPrefixes[] = {
    "claude", "anthropic", "openai", "google", "meta", "mistralai",
    "deepseek-ai", "alibaba", "qwen-team", "x-ai", "xai",
};

bool is_vendor_token(std::string_view t) {
    for (const auto v : kVendorPrefixes)
        if (lower_eq(t, v)) return true;
    return false;
}

// The dotted version implied by a decoded generation/revision. Reuses
// from_id's already-validated numerics rather than re-scanning the string —
// which is exactly how the old third version-extractor came to disagree with
// the other two about dates.
std::string version_from(const ModelCapabilities& caps) {
    if (caps.generation <= 0) return {};
    std::string v = std::to_string(caps.generation);
    if (caps.revision > 0) {
        v.push_back('.');
        v.append(std::to_string(caps.revision));
    }
    return v;
}

// Split an id into '-'/'.'-delimited tokens, with the provider namespace and
// any Ollama `:tag` already removed. Used for the two residual facts that
// from_id does NOT expose, both of which are POSITION-dependent and so
// cannot be recovered from the capability struct alone.
std::vector<std::string_view> id_tokens(std::string_view id) {
    if (const auto slash = id.find_last_of('/');
        slash != std::string_view::npos)
        id.remove_prefix(slash + 1);
    if (const auto colon = id.find(':'); colon != std::string_view::npos)
        id = id.substr(0, colon);

    std::vector<std::string_view> t;
    std::size_t start = 0;
    for (std::size_t i = 0; i <= id.size(); ++i) {
        bool boundary = (i == id.size() || id[i] == '-');
        // A '.' splits a VERSION (`gpt-5.1` → 5 | 1) but not a name it is
        // glued to (`llama3.3` → Llama3.3, `qwen2.5` → Qwen2.5). The
        // distinguishing test is whether the token so far is pure digits: in
        // `5.1` it is, in `llama3.3` it isn't. Splitting unconditionally
        // turned "Llama3.3" into "Llama3 3".
        if (!boundary && id[i] == '.') {
            const std::string_view sofar = id.substr(start, i - start);
            boundary = all_digits(sofar);
        }
        if (!boundary) continue;
        if (i > start) t.push_back(id.substr(start, i - start));
        start = i + 1;
    }
    return t;
}

// Index of the token that names the family, or npos.
std::size_t family_token_index(const std::vector<std::string_view>& toks) {
    for (std::size_t i = 0; i < toks.size(); ++i)
        if (lower_eq(toks[i], "haiku") || lower_eq(toks[i], "sonnet")
         || lower_eq(toks[i], "opus")  || lower_eq(toks[i], "fable")
         || lower_eq(toks[i], "mythos") || lower_eq(toks[i], "gpt"))
            return i;
    return std::string_view::npos;
}

// LEGACY SCHEMA RECOVERY.
//
// from_id reads the generation from the token AFTER the family word, which
// is the modern schema (`claude-sonnet-4-5`). The legacy schema puts it
// BEFORE (`claude-3-5-haiku-20241022`), so caps.generation is 0 there —
// catalog.hpp's max_output_tokens_for() documents exactly this and works
// around it with a `find("claude-3")` prefix sniff.
//
// Without this, `claude-3-5-haiku` renders as bare "Haiku", losing the 3.5
// and making it indistinguishable from a 4.5 Haiku in a picker row. Read the
// short pure-digit run immediately preceding the family word.
std::string version_before_family(const std::vector<std::string_view>& toks,
                                  std::size_t fam) {
    if (fam == std::string_view::npos || fam == 0) return {};
    std::size_t first = fam;
    while (first > 0 && all_digits(toks[first - 1])
           && toks[first - 1].size() <= 2)
        --first;
    if (first == fam) return {};

    std::string v;
    for (std::size_t i = first; i < fam; ++i) {
        if (!v.empty()) v.push_back('.');
        v.append(toks[i]);
    }
    return v;
}

// QUALIFIER RECOVERY.
//
// A family + version is not always the whole identity: `gpt-5.1-codex-max`
// and `gpt-5.1` are different products, and rendering both as "GPT 5.1"
// would make the picker ambiguous about the model you are actually about to
// pay for. Collect the trailing alphabetic tokens that follow the family and
// its version run — "Codex Max", "Chat", "Sol" — dropping provenance
// (snapshots, alias pointers) exactly as normalize_id does.
std::string qualifier_after_family(const std::vector<std::string_view>& toks,
                                   std::size_t fam) {
    if (fam == std::string_view::npos) return {};

    // Skip the family word, then any short numeric version tokens.
    std::size_t i = fam + 1;
    while (i < toks.size() && all_digits(toks[i]) && toks[i].size() <= 2)
        ++i;

    std::string out;
    for (; i < toks.size(); ++i) {
        const std::string_view t = toks[i];
        if (is_snapshot_word(t)) continue;              // 20250514
        if (all_digits(t) && t.size() >= 3) continue;   // date fragment
        if (lower_eq(t, "latest")) continue;            // alias pointer
        if (!out.empty()) out.push_back(' ');
        append_word(out, t);
    }
    return out;
}

// UNKNOWN-FAMILY STRUCTURAL DECODE.
//
// A known family gets its name from the table and its version from from_id.
// An unknown one has neither — but it is NOT therefore unstructured. Model
// ids overwhelmingly follow `[vendor-]name-version[-qualifier…]`, so we can
// recover the same three fields and give unknown models the same shed ladder
// and the same typography as known ones.
//
// This is what makes the decoder degrade gracefully rather than fall off a
// cliff. Before it, `gemini-4-pro` produced one flat string for all three
// rungs — so the shed ladder was a no-op exactly where names are LONGEST and
// shedding matters most — and a future `claude-quasar-6` rendered with a
// vendor prefix no current model uses.
//
// Fills `out.name` / `.version` / `.qualifier`. Never leaves name empty for
// a non-empty id.
void decode_unknown(std::string_view id, ModelName& out) {
    // The Ollama `:tag` carries what a human distinguishes local models BY —
    // size (`70b`) and variant (`instruct`) — and id_tokens() drops it, so
    // recover it separately and append it to the qualifier. Losing it would
    // collapse `llama3:8b` and `llama3:70b` to the same label.
    std::string tag_words;
    if (const auto colon = id.find(':'); colon != std::string_view::npos) {
        std::string_view tag = id.substr(colon + 1);
        if (!lower_eq(tag, "latest")) {
            std::size_t p0 = 0;
            for (std::size_t i = 0; i <= tag.size(); ++i) {
                if (i != tag.size() && tag[i] != '-') continue;
                const std::string_view part = tag.substr(p0, i - p0);
                p0 = i + 1;
                if (part.empty() || is_quant(part)) continue;
                if (!tag_words.empty()) tag_words.push_back(' ');
                append_word(tag_words, part);
            }
        }
    }
    const auto attach_tag = [&](std::string& q) {
        if (tag_words.empty()) return;
        if (!q.empty()) q.push_back(' ');
        q.append(tag_words);
    };

    auto toks = id_tokens(id);

    // Leading vendor tokens are provenance, not name.
    std::size_t b = 0;
    while (b < toks.size() && is_vendor_token(toks[b])) ++b;
    // ...unless that consumed everything (`claude` alone): keep the last one
    // so the name is never empty.
    if (b == toks.size() && b > 0) --b;

    // Trailing provenance: snapshot dates, date fragments, alias pointers.
    std::size_t e = toks.size();
    while (e > b + 1) {
        const std::string_view t = toks[e - 1];
        if (is_snapshot_word(t) || lower_eq(t, "latest")
            || (all_digits(t) && t.size() >= 3)) { --e; continue; }
        break;
    }
    // A `YYYY-MM-DD` triple spelled as three tokens (gpt-4o-2024-08-06).
    if (e >= b + 4) {
        const auto y = toks[e - 3], mo = toks[e - 2], d = toks[e - 1];
        if (y.size() == 4 && all_digits(y) && y.substr(0, 2) == "20"
            && mo.size() == 2 && all_digits(mo)
            && d.size() == 2 && all_digits(d))
            e -= 3;
    }

    if (b >= e) {                       // nothing survived: fall back whole
        out.name = normalize_id(id);
        return;
    }
    // The NAME runs until the first standalone version token — a short pure
    // digit run, or a digit-led token that is not itself part of the name
    // (`4o` in `gpt-4o` IS the name's version; `2.5` in `qwen2.5` is glued to
    // it by the tokeniser and never appears standalone).
    std::size_t v = b;
    while (v < e && !(all_digits(toks[v]) && toks[v].size() <= 2)) ++v;

    std::string name;
    for (std::size_t i = b; i < v; ++i) {
        if (!name.empty()) name.push_back(' ');
        append_word(name, toks[i]);
    }

    // VERSION: the run of short digit tokens, dotted.
    std::size_t q = v;
    std::string version;
    while (q < e && all_digits(toks[q]) && toks[q].size() <= 2) {
        if (!version.empty()) version.push_back('.');
        version.append(toks[q]);
        ++q;
    }

    // QUALIFIER: whatever follows the version.
    std::string qualifier;
    for (std::size_t i = q; i < e; ++i) {
        if (!qualifier.empty()) qualifier.push_back(' ');
        append_word(qualifier, toks[i]);
    }

    // An id that is ONLY a version (`4-5`) has no name; keep the normalized
    // whole rather than emitting a bare number.
    if (name.empty()) {
        out.name = normalize_id(id);
        return;
    }

    attach_tag(qualifier);
    out.name      = std::move(name);
    out.version   = std::move(version);
    out.qualifier = std::move(qualifier);
}

} // namespace

// ── decode ───────────────────────────────────────────────────────────────

ModelName decode(std::string_view id, std::string_view server_name) {
    ModelName out;
    if (id.empty()) return out;

    // ── 0. Harden the inputs, ONCE, before anything parses them ──────
    //
    // Everything below this point may assume its input is control-free and
    // bounded. That assumption is what lets the rest of the file stay simple:
    // no parser, projection or widget has to re-check, and there is no call
    // site that can forget to.
    const std::string safe_id   = strip_controls(
        id.substr(0, std::min(id.size(), kMaxIdLen)));
    const std::string safe_name = strip_controls(
        server_name.substr(0, std::min(server_name.size(), kMaxIdLen)));
    id          = safe_id;
    server_name = safe_name;
    if (id.empty()) return out;   // the id was ENTIRELY control bytes

    // Clamp every field on the way out, whichever branch produced it. Doing
    // it here rather than in each branch means a future branch cannot
    // reintroduce an unbounded label.
    const auto finish = [](ModelName n) {
        n.name      = clamp_len(std::move(n.name));
        n.qualifier = clamp_len(std::move(n.qualifier));
        n.version   = clamp_len(std::move(n.version));
        return n;
    };

    // ── 1. Peel the agentty extended-context marker ───────────────────
    // `[1m]`/`[2m]` are selection-time markers (wire_model_id strips them
    // before the id reaches the wire). They must not leak as literal text,
    // but they must not be silently DROPPED either: the variant is the whole
    // point of its picker row, and a user who enabled a 1M window needs to
    // see that they got it. So it becomes structured data.
    struct Marker { std::string_view tag, shown; };
    std::string bare;
    for (const Marker& mk : {Marker{"[1m]", "1M"}, Marker{"[2m]", "2M"}}) {
        const auto pos = id.find(mk.tag);
        if (pos == std::string_view::npos) continue;
        bare.assign(id.substr(0, pos));
        bare.append(id.substr(pos + mk.tag.size()));
        out.annotation = std::string{mk.shown};
        id = bare;
        break;
    }

    // ── 2. Taxonomy: delegated, never re-derived ────────────────────────
    const auto caps = ModelCapabilities::from_id(id);
    out.color        = color_of(caps.family);
    out.family_known = caps.is_known_family();

    if (out.family_known) {
        out.name    = std::string{family_label(caps.family)};
        out.version = version_from(caps);

        // Two residual facts from_id cannot express, both POSITIONAL:
        //
        //   • the legacy schema puts the version BEFORE the family word
        //     (claude-3-5-haiku), where from_id's post-family read finds
        //     nothing — leaving bare "Haiku" for a 3.5 model;
        //   • a product qualifier can follow the version (gpt-5.1-codex-max),
        //     and dropping it makes two different products render alike.
        //
        // Tokenised once, here, and ONLY consulted for what the capability
        // struct genuinely does not carry — the taxonomy itself still comes
        // exclusively from from_id.
        const auto toks = id_tokens(id);
        const auto fam  = family_token_index(toks);
        if (out.version.empty())
            out.version = version_before_family(toks, fam);
        out.qualifier = qualifier_after_family(toks, fam);
        return finish(std::move(out));
    }

    // ── 3. Unknown family: recover structure from the id itself ───────
    //
    // NOT a dead end. Model ids overwhelmingly follow
    // `[vendor-]name-version[-qualifier]`, so an unknown model gets the same
    // three fields, the same shed ladder and the same vendor-free typography
    // as a known one — it just sourced them from the string instead of the
    // table. That is what makes shipping a new family a NON-EVENT for the UI:
    // `claude-quasar-6` reads "Quasar 6", exactly like `claude-opus-4-8`
    // reads "Opus 4.8", months before anyone adds Quasar to the enum.
    decode_unknown(id, out);

    if (server_name.empty() || server_name == id) return finish(std::move(out));

    // Is the server name merely a re-cased / re-punctuated spelling of the
    // id? Then prefer the id-derived form so the same model reads
    // identically across providers. Only a genuine marketing alias the id
    // cannot reconstruct ("Nano Banana Pro" for gemini-3-pro-image) wins —
    // and even then it is normalized, so its own cruft ((latest), casing)
    // is cleaned to match the rest of the list's typography.
    //
    // A marketing alias is a NAME, not a name+version+qualifier triple
    // ("Nano Banana Pro" has no version to split out), so it replaces the
    // whole decode rather than merging into it — merging would leave a
    // version recovered from the id dangling off an unrelated alias.
    const std::string from_id   = out.full();
    const std::string from_name = normalize_id(server_name);
    const bool same_model = skeleton(from_id) == skeleton(from_name)
                         || skeleton(server_name) == skeleton(id);
    if (!same_model) {
        out.name      = from_name;
        out.version.clear();
        out.qualifier.clear();
    }
    return finish(std::move(out));
}

// ── Projections ──────────────────────────────────────────────────────────
//
// Strictly nested by construction, each rung adding exactly one field:
//
//   tiny()   = name + version
//   medium() = tiny()   + qualifier
//   full()   = medium() + annotation
//
// Written as literal composition so the nesting is evident on the page
// rather than an invariant to remember — a narrow surface can never state
// something a wide one contradicts, because it is a prefix of it.

std::string ModelName::tiny() const {
    std::string s = name;
    if (!version.empty()) {
        s.push_back(' ');
        s.append(version);
    }
    return s;
}

std::string ModelName::medium() const {
    std::string s = tiny();
    if (!qualifier.empty()) {
        s.push_back(' ');
        s.append(qualifier);
    }
    return s;
}

std::string ModelName::full() const {
    std::string s = medium();
    if (!annotation.empty()) {
        s.append(" \xc2\xb7 ");   // " · "
        s.append(annotation);
    }
    return s;
}

} // namespace agentty::model_name
