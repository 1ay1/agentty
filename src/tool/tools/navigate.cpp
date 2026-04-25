// `navigate(question)` — semantic finder for huge codebases.
//
// At 100k-file scale the agent's biggest pain is "I don't know where
// to start." `grep` returns thousands of hits; `repo_map` shows
// modules but not the answer; `find_definition` needs the exact name.
//
// `navigate` takes a natural-language question and returns the top
// candidate files, symbols, and memos to look at — no reads, no
// grep, no sub-agent. Pure-C++ scoring runs in milliseconds.
//
// Scoring layers (each ranked, then merged):
//   1. Symbols: token-overlap × centrality × recency-of-defining-file
//   2. Files:   sum of contained-symbol scores + filename token match
//      + per-file knowledge-card text token match
//   3. Memos:   bag-of-words overlap with topic + body
//   4. Modules: sum of contained-file scores
//
// Output is a ranked, multi-section answer the agent can act on
// directly: top files to read/outline, top symbols to find_usages on,
// and any memos that already cover the question.

#include "moha/index/repo_index.hpp"
#include "moha/memory/file_card_store.hpp"
#include "moha/memory/memo_store.hpp"
#include "moha/tool/spec.hpp"
#include "moha/tool/tools.hpp"
#include "moha/tool/util/arg_reader.hpp"
#include "moha/tool/util/fs_helpers.hpp"
#include "moha/tool/util/tool_args.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <set>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <nlohmann/json.hpp>

namespace moha::tools {

using json = nlohmann::json;
namespace fs = std::filesystem;

namespace {

struct NavigateArgs {
    std::string question;
    int         limit;          // top-N files to surface
    std::string display_description;
};

[[nodiscard]] std::expected<NavigateArgs, ToolError>
parse_navigate_args(const json& j) {
    util::ArgReader ar(j);
    auto q = ar.require_str("question");
    if (!q) return std::unexpected(ToolError::invalid_args(
        "question required: a natural-language description of what you're looking for"));
    int limit = ar.integer("limit", 6);
    if (limit < 1)  limit = 1;
    if (limit > 20) limit = 20;
    return NavigateArgs{*std::move(q), limit, ar.str("display_description", "")};
}

// Tokenize: lowercase, ≥3 char alphanumeric runs, plus split on
// camelCase / snake_case boundaries so a query like "auth flow"
// matches "authFlow", "AuthFlow", "auth_flow", "authenticate".
[[nodiscard]] std::unordered_set<std::string>
tokenize_rich(std::string_view s) {
    std::unordered_set<std::string> out;
    auto add = [&](std::string w) {
        if (w.size() >= 2) {
            std::ranges::transform(w, w.begin(),
                [](unsigned char c){ return static_cast<char>(std::tolower(c)); });
            out.insert(std::move(w));
        }
    };
    std::string cur;
    auto flush = [&]() { if (!cur.empty()) { add(std::move(cur)); cur.clear(); } };
    for (std::size_t i = 0; i < s.size(); ++i) {
        unsigned char u = static_cast<unsigned char>(s[i]);
        if (std::isalnum(u) || u == '_') {
            // Detect camelCase boundary: lowercase→uppercase.
            if (!cur.empty() && std::isupper(u)
                && std::islower(static_cast<unsigned char>(cur.back()))) {
                flush();
            }
            cur.push_back(static_cast<char>(u));
        } else {
            flush();
        }
    }
    flush();
    // Common English noise — skip.
    static const std::unordered_set<std::string> stop = {
        "the","and","for","with","that","this","what","where","when",
        "how","does","into","from","over","of","is","in","on","to",
        "an","a","be","or","do","as","by","at","it",
    };
    std::erase_if(out, [&](const std::string& w) { return stop.contains(w); });
    return out;
}

// Score = number of query tokens present in candidate text.
[[nodiscard]] int
overlap_score(const std::unordered_set<std::string>& q,
              const std::unordered_set<std::string>& candidate) {
    int hits = 0;
    for (const auto& w : q) if (candidate.contains(w)) ++hits;
    return hits;
}

// Substring score: each query token that appears in the lowercased
// candidate string. Catches matches the tokenizer misses (e.g.
// "auth" vs "authentication").
[[nodiscard]] int
substring_score(const std::unordered_set<std::string>& q,
                std::string_view candidate) {
    std::string lc{candidate};
    std::ranges::transform(lc, lc.begin(),
        [](unsigned char c){ return static_cast<char>(std::tolower(c)); });
    int hits = 0;
    for (const auto& w : q)
        if (lc.find(w) != std::string::npos) ++hits;
    return hits;
}

ExecResult run_navigate(const NavigateArgs& a) {
    auto& idx = index::shared();
    if (!idx.ready())
        idx.refresh(fs::current_path());

    auto qt = tokenize_rich(a.question);
    if (qt.empty())
        return std::unexpected(ToolError::invalid_args(
            "question contains no usable terms — try natural-language words"));

    fs::path root = idx.workspace();

    // ── Score symbols ───────────────────────────────────────────
    // Each symbol scored by:
    //   token overlap × 10 + substring × 4 + centrality + 1
    // The "+1" floor lets symbols with no overlap-but-high-centrality
    // surface near the bottom; they're still reasonable suggestions.
    struct ScoredSymbol {
        std::string  name;
        int          score = 0;
        int          centrality = 0;
        fs::path     defining_file;
    };
    std::vector<ScoredSymbol> sym_hits;
    {
        const auto& scores = idx.symbol_scores();
        // Walk every defined name we know. To find the defining file
        // we re-walk all_files() — O(symbols + files). For a 100k
        // file repo this is ~1M comparisons but still <50 ms.
        std::unordered_map<std::string, fs::path> first_def;
        for (const auto& fi : idx.all_files())
            for (const auto& s : fi.symbols)
                first_def.try_emplace(s.name, fi.path);

        for (const auto& [name, centrality] : scores) {
            if (name.size() < 3) continue;
            auto sym_tokens = tokenize_rich(name);
            int over = overlap_score(qt, sym_tokens);
            int sub  = substring_score(qt, name);
            if (over == 0 && sub == 0) continue;
            int score = over * 10 + sub * 4 + std::min(centrality, 50);
            ScoredSymbol ss{name, score, centrality, {}};
            if (auto it = first_def.find(name); it != first_def.end())
                ss.defining_file = it->second;
            sym_hits.push_back(std::move(ss));
        }
    }
    std::ranges::sort(sym_hits, [](const auto& a_, const auto& b_) {
        return a_.score > b_.score;
    });
    if (sym_hits.size() > 8) sym_hits.resize(8);

    // ── Score files ─────────────────────────────────────────────
    // file_score = filename-token overlap × 8
    //            + sum_of(symbol_score where symbol is defined here) × 0.5
    //            + card-text token overlap × 4
    //            + recency boost (existing in fi.score)
    struct ScoredFile {
        fs::path path;
        int      score = 0;
        std::string why;     // "matches: auth, flow", "centrality 412"
    };
    std::vector<ScoredFile> file_hits;
    {
        auto files = idx.all_files();
        for (auto& fi : files) {
            auto rel = fs::relative(fi.path, root).generic_string();
            // Filename-token match.
            auto name_tokens = tokenize_rich(rel);
            int name_over = overlap_score(qt, name_tokens);
            int name_sub  = substring_score(qt, rel);

            // Symbol score for symbols defined in this file that
            // matched the query.
            int symbol_contribution = 0;
            std::vector<std::string> matched_syms;
            for (const auto& sh : sym_hits) {
                if (sh.defining_file == fi.path) {
                    symbol_contribution += sh.score;
                    matched_syms.push_back(sh.name);
                }
            }

            // Card text overlap (semantic-ish via summary words).
            int card_over = 0;
            if (auto card = memory::shared_cards().get(fi.path);
                card && !card->summary.empty()) {
                card_over = substring_score(qt, card->summary);
            }

            int total = name_over * 8 + name_sub * 4
                      + symbol_contribution / 2
                      + card_over * 6
                      + fi.score / 4;
            if (total <= 0) continue;
            ScoredFile sf{fi.path, total, ""};
            std::ostringstream why;
            if (name_over + name_sub > 0)
                why << "name matches " << (name_over + name_sub) << "  ";
            if (!matched_syms.empty()) {
                why << "defines ";
                for (std::size_t i = 0; i < matched_syms.size() && i < 2; ++i) {
                    if (i) why << "/";
                    why << matched_syms[i];
                }
                why << "  ";
            }
            if (card_over > 0) why << "card matches " << card_over << "  ";
            sf.why = why.str();
            file_hits.push_back(std::move(sf));
        }
    }
    std::ranges::sort(file_hits, [](const auto& a_, const auto& b_) {
        return a_.score > b_.score;
    });
    if (static_cast<int>(file_hits.size()) > a.limit)
        file_hits.resize(a.limit);

    // ── Score memos ─────────────────────────────────────────────
    auto memo_hit = memory::shared().find_similar(a.question);

    // ── Modules: dirs whose top-symbols/path match ──────────────
    auto modules = idx.detect_modules();
    struct ScoredModule {
        std::string name;
        std::string dir;
        int         score = 0;
    };
    std::vector<ScoredModule> mod_hits;
    for (const auto& m : modules) {
        std::string dir_str = fs::relative(m.dir, root).generic_string();
        int over = overlap_score(qt, tokenize_rich(dir_str));
        int sub  = substring_score(qt, dir_str);
        // Sum overlap from top_symbols of this module.
        int sym_over = 0;
        for (const auto& s : m.top_symbols)
            sym_over += overlap_score(qt, tokenize_rich(s));
        int total = over * 6 + sub * 3 + sym_over * 2;
        if (total <= 0) continue;
        mod_hits.push_back({m.name, std::move(dir_str), total});
    }
    std::ranges::sort(mod_hits, [](const auto& a_, const auto& b_) {
        return a_.score > b_.score;
    });
    if (mod_hits.size() > 4) mod_hits.resize(4);

    // ── Render ───────────────────────────────────────────────────
    if (file_hits.empty() && sym_hits.empty()
        && mod_hits.empty() && !memo_hit) {
        return ToolOutput{
            "No structural matches for that question. Try `signatures("
            + a.question + ")`, `grep`, or `investigate(\"" + a.question
            + "\")` for a deeper search.",
            std::nullopt};
    }
    std::ostringstream out;
    out << "Navigation results for: \"" << a.question << "\"\n";

    if (!mod_hits.empty()) {
        out << "\n## Likely modules\n";
        for (const auto& m : mod_hits) {
            out << "  [" << m.score << "] " << m.dir << "/   ("
                << m.name << ")\n";
        }
    }
    if (!file_hits.empty()) {
        out << "\n## Top files\n";
        for (const auto& f : file_hits) {
            std::error_code ec;
            auto rel = fs::relative(f.path, root, ec).generic_string();
            out << "  [" << f.score << "] " << rel << "\n";
            if (!f.why.empty())
                out << "         (" << f.why << ")\n";
        }
    }
    if (!sym_hits.empty()) {
        out << "\n## Top symbols\n";
        for (const auto& s : sym_hits) {
            std::error_code ec;
            auto rel = s.defining_file.empty()
                ? std::string{"?"}
                : fs::relative(s.defining_file, root, ec).generic_string();
            out << "  [" << s.score << "] " << s.name
                << "   defined in " << rel
                << "  (" << s.centrality << " refs)\n";
        }
    }
    if (memo_hit) {
        int conf = memo_hit->effective_confidence(idx.workspace());
        out << "\n## Related memo\n"
            << "  [" << conf << "%] " << memo_hit->query
            << "  (call `recall(\"" << memo_hit->id << "\")` for full text)\n";
    }
    out << "\nUse `outline(path)` on a file to see its symbols, "
        << "`find_usages(name)` on a symbol, or `read(path)` for source.";

    std::string body = out.str();
    if (!a.display_description.empty())
        body = a.display_description + "\n\n" + body;
    return ToolOutput{std::move(body), std::nullopt};
}

} // namespace

ToolDef tool_navigate() {
    ToolDef t;
    constexpr const auto& kSpec = spec::require<"navigate">();
    t.name = ToolName{std::string{kSpec.name}};
    t.description =
        "Semantic codebase finder. Given a natural-language question, "
        "returns ranked candidate FILES, SYMBOLS, MODULES, and MEMOS "
        "to look at — without any sub-agent or grep. Pure-C++ scoring "
        "over the symbol index + file knowledge cards + memo store, "
        "runs in milliseconds even on huge codebases. Use BEFORE "
        "`investigate` / `grep` / `read` when you don't know where "
        "the relevant code lives. Examples:\n"
        "  navigate(\"where do we handle webhook delivery retries?\")\n"
        "  navigate(\"what does authentication look like?\")\n"
        "  navigate(\"how is the streaming retry state machine wired?\")\n"
        "Returns a structured answer; the agent then `outline`s / "
        "`read`s / `find_usages`s the candidates it cares about.";
    t.input_schema = json{
        {"type","object"},
        {"required", {"question"}},
        {"properties", {
            {"display_description", {{"type","string"},
                {"description","One-line summary shown in the UI. Optional."}}},
            {"question", {{"type","string"},
                {"description","Natural-language description of what you're trying to find."}}},
            {"limit", {{"type","integer"},
                {"description","Max files surfaced (default 6, max 20)."}}},
        }},
    };
    t.effects = kSpec.effects;
    t.eager_input_streaming = kSpec.eager_input_streaming;
    t.execute = util::adapt<NavigateArgs>(parse_navigate_args, run_navigate);
    return t;
}

} // namespace moha::tools
