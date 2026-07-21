// agentty::rag — BM25, RRF, cosine, and the document chunker.
// Pure C++/STL; no third-party deps. See rag.hpp for the design rationale.

#include "agentty/rag/rag.hpp"
#include "agentty/rag/simd.hpp"
#include "agentty/rag/stemmer.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <span>
#include <cstdlib>
#include <unordered_map>

namespace agentty::rag {

namespace {

// Porter stemming collapses morphological variants ("deploy/deployment/
// deploying", "run/runs/running") to one term — a real recall win on
// natural-language docs, which is the common case for this corpus. ON by
// default; set BM25_USE_STEMMER=0 to disable (e.g. a code-symbol corpus where
// over-conflation hurts). Read ONCE and cached — the same setting MUST hold
// for index build AND query (both go through tokenize() below), so a
// process-lifetime constant is exactly right. BM25 postings are never
// persisted (rebuilt from chunk text every build), so flipping this is safe
// against stale caches.
bool stemmer_enabled() noexcept {
    static const bool on = [] {
        const char* v = std::getenv("BM25_USE_STEMMER");
        if (!v || !v[0]) return true;   // default ON
        return v[0] != '0' &&
               std::string_view{v} != "false" && std::string_view{v} != "FALSE";
    }();
    return on;
}

// Lowercase alphanumeric-run tokenizer. Tokens shorter than 2 chars are
// dropped (single letters carry no retrieval signal). When BM25_USE_STEMMER
// is set, each token is Porter-stemmed; applied identically at index + query
// time so postings and probe terms stay in the same vocabulary.
void tokenize(std::string_view s, std::vector<std::string>& out) {
    const bool do_stem = stemmer_enabled();
    std::string cur;
    cur.reserve(24);
    auto flush = [&] {
        if (cur.size() >= 2) out.push_back(do_stem ? stem(cur) : cur);
        cur.clear();
    };
    for (unsigned char c : s) {
        if (std::isalnum(c)) cur.push_back(static_cast<char>(std::tolower(c)));
        else                 flush();
    }
    flush();
}

// BM25 parameters (standard defaults).
constexpr double kK1 = 1.5;   // term-frequency saturation
constexpr double kB  = 0.75;  // length normalization

// How many copies of the heading breadcrumb to fold into a chunk's token bag
// (field boost). 3 total is a modest, well-behaved default; 1 disables the
// boost (breadcrumb still indexed once). Read once, cached.
int heading_boost() {
    static const int v = [] {
        if (const char* e = std::getenv("BM25_HEADING_BOOST"); e && e[0]) {
            int n = std::atoi(e);
            if (n >= 1 && n <= 16) return n;
        }
        return 3;
    }();
    return v;
}

} // namespace

void Bm25Index::clear() {
    postings.clear();
    doc_len.clear();
    term_ids.clear();
    avg_doc_len = 0.0;
    doc_count = 0;
}

Bm25Index build_bm25(const std::vector<Chunk>& chunks) {
    Bm25Index idx;
    idx.doc_count = chunks.size();
    idx.doc_len.resize(chunks.size(), 0);

    std::vector<std::string> toks;
    std::uint64_t total_len = 0;

    for (std::uint32_t d = 0; d < chunks.size(); ++d) {
        toks.clear();
        tokenize(chunks[d].text, toks);
        // CONTEXTUAL BM25 (Anthropic contextual retrieval): the situating
        // breadcrumb participates in the index, so heading/document terms
        // match chunks whose bodies never repeat them. Tokenized once here
        // — zero cost at query time.
        //
        // FIELD BOOST: a query term matching in a HEADING is a much stronger
        // relevance signal than one buried in body prose ("# Installation" vs
        // the word "install" in a sentence). BM25 has no field concept, so we
        // boost the standard way — repeat the heading tokens kHeadingBoost
        // times, raising their tf. This also lifts doc_len, but avgdl
        // normalization amortizes that across the corpus, so the net effect is
        // heading terms out-scoring equally-frequent body terms. Tunable via
        // BM25_HEADING_BOOST (default 3 total copies; 1 = no boost).
        if (!chunks[d].context.empty()) {
            const int copies = heading_boost();
            for (int r = 0; r < copies; ++r)
                tokenize(chunks[d].context, toks);
        }
        idx.doc_len[d] = static_cast<std::uint32_t>(toks.size());
        total_len += toks.size();

        // Term frequencies within this doc.
        std::unordered_map<std::uint32_t, std::uint32_t> tf;
        tf.reserve(toks.size());
        for (const auto& t : toks) {
            auto it = idx.term_ids.find(t);
            std::uint32_t id;
            if (it == idx.term_ids.end()) {
                id = static_cast<std::uint32_t>(idx.term_ids.size());
                idx.term_ids.emplace(t, id);
                idx.postings.emplace_back();
            } else {
                id = it->second;
            }
            ++tf[id];
        }
        for (auto [id, count] : tf)
            idx.postings[id].push_back({d, count});
    }

    idx.avg_doc_len = chunks.empty() ? 0.0
        : static_cast<double>(total_len) / static_cast<double>(chunks.size());
    return idx;
}

std::vector<std::pair<std::uint32_t, double>>
bm25_search(const Bm25Index& idx, std::string_view query, std::size_t k) {
    std::vector<std::string> qtoks;
    tokenize(query, qtoks);

    std::unordered_map<std::uint32_t, double> scores;
    const double N = static_cast<double>(idx.doc_count);
    if (N <= 0.0) return {};

    for (const auto& qt : qtoks) {
        auto it = idx.term_ids.find(qt);
        if (it == idx.term_ids.end()) continue;
        const auto& plist = idx.postings[it->second];
        const double df = static_cast<double>(plist.size());
        if (df <= 0.0) continue;
        // BM25 IDF with +0.5 smoothing; clamp to >=0 so a term present in
        // >half the docs can't push a chunk's score negative.
        double idf = std::log((N - df + 0.5) / (df + 0.5) + 1.0);
        if (idf < 0.0) idf = 0.0;

        for (const auto& p : plist) {
            const double tf  = static_cast<double>(p.tf);
            const double dl  = static_cast<double>(idx.doc_len[p.doc]);
            const double adl = idx.avg_doc_len > 0 ? idx.avg_doc_len : 1.0;
            const double norm = tf * (kK1 + 1.0) /
                (tf + kK1 * (1.0 - kB + kB * dl / adl));
            scores[p.doc] += idf * norm;
        }
    }

    std::vector<std::pair<std::uint32_t, double>> out(scores.begin(), scores.end());
    std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) {
        if (a.second != b.second) return a.second > b.second;
        return a.first < b.first;  // stable tiebreak by chunk id
    });
    if (out.size() > k) out.resize(k);
    return out;
}

double cosine(const std::vector<float>& a, const std::vector<float>& b) noexcept {
    if (a.size() != b.size() || a.empty()) return 0.0;
    // Dense brute-force similarity (used below the HNSW threshold). All three
    // accumulators are dot products, so route them through the runtime-
    // dispatched SIMD path. Accumulate in float (SIMD) then widen for the
    // final divide — ample precision for ranking 768-dim unit-ish vectors.
    // Span overloads carry length with the data (sizes already equal above).
    const double dot = static_cast<double>(simd::dot(std::span<const float>(a), std::span<const float>(b)));
    const double na  = static_cast<double>(simd::dot(std::span<const float>(a), std::span<const float>(a)));
    const double nb  = static_cast<double>(simd::dot(std::span<const float>(b), std::span<const float>(b)));
    if (na <= 0.0 || nb <= 0.0) return 0.0;
    return dot / (std::sqrt(na) * std::sqrt(nb));
}

std::vector<std::pair<std::uint32_t, double>>
reciprocal_rank_fusion_weighted(
    const std::vector<std::vector<std::uint32_t>>& ranked_lists,
    const std::vector<double>& weights,
    double k, std::size_t out_k) {
    std::unordered_map<std::uint32_t, double> fused;
    for (std::size_t li = 0; li < ranked_lists.size(); ++li) {
        const auto& list = ranked_lists[li];
        const double w = (li < weights.size()) ? weights[li] : 1.0;
        if (w == 0.0) continue;
        for (std::size_t rank = 0; rank < list.size(); ++rank) {
            // rank is 0-based; RRF uses 1-based rank.
            fused[list[rank]] += w / (k + static_cast<double>(rank + 1));
        }
    }
    std::vector<std::pair<std::uint32_t, double>> out(fused.begin(), fused.end());
    std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) {
        if (a.second != b.second) return a.second > b.second;
        return a.first < b.first;
    });
    if (out.size() > out_k) out.resize(out_k);
    return out;
}

std::vector<std::pair<std::uint32_t, double>>
reciprocal_rank_fusion(
    const std::vector<std::vector<std::uint32_t>>& ranked_lists,
    double k, std::size_t out_k) {
    // The unweighted fusion is the weighted one with all weights == 1.0.
    return reciprocal_rank_fusion_weighted(ranked_lists, /*weights=*/{}, k, out_k);
}

std::vector<std::pair<std::uint32_t, double>>
relative_score_fusion_weighted(
    const std::vector<std::vector<std::pair<std::uint32_t, double>>>& scored_lists,
    const std::vector<double>& weights,
    std::size_t out_k) {
    std::unordered_map<std::uint32_t, double> fused;
    for (std::size_t li = 0; li < scored_lists.size(); ++li) {
        const auto& list = scored_lists[li];
        if (list.empty()) continue;
        const double w = (li < weights.size()) ? weights[li] : 1.0;
        if (w == 0.0) continue;
        // Min-max over THIS list's raw scores so lists on different scales
        // (unbounded BM25 vs cosine in [-1,1]) become comparable before the
        // weighted sum. A degenerate all-equal list maps every member to 1.0
        // (it still votes, just without internal ordering).
        double lo = list.front().second, hi = list.front().second;
        for (const auto& [id, s] : list) { lo = std::min(lo, s); hi = std::max(hi, s); }
        const double span = hi - lo;
        for (const auto& [id, s] : list) {
            const double norm = span > 0.0 ? (s - lo) / span : 1.0;
            fused[id] += w * norm;
        }
    }
    std::vector<std::pair<std::uint32_t, double>> out(fused.begin(), fused.end());
    std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) {
        if (a.second != b.second) return a.second > b.second;
        return a.first < b.first;
    });
    if (out.size() > out_k) out.resize(out_k);
    return out;
}

// ── Chunker ──────────────────────────────────────────────────────────────────────

namespace {

bool is_heading(std::string_view line) {
    // Markdown ATX heading; cheap heuristic for a semantic break point.
    std::size_t i = 0;
    while (i < line.size() && (line[i] == ' ' || line[i] == '	')) ++i;
    return i < line.size() && line[i] == '#';
}

bool is_fenced_code_start(std::string_view line) {
    // Markdown fenced code block start: ``` or ~~~
    std::size_t i = 0;
    while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) ++i;
    if (i + 2 >= line.size()) return false;
    return (line.substr(i, 3) == "```" || line.substr(i, 3) == "~~~");
}

bool is_list_item(std::string_view line) {
    // Markdown list item: -, *, +, or numbered (1.)
    std::size_t i = 0;
    while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) ++i;
    if (i >= line.size()) return false;
    char c = line[i];
    if (c == '-' || c == '*' || c == '+') {
        return i + 1 < line.size() && line[i + 1] == ' ';
    }
    // Numbered list: digit(s) followed by . or )
    std::size_t j = i;
    while (j < line.size() && std::isdigit((unsigned char)line[j])) ++j;
    if (j > i && j < line.size() && (line[j] == '.' || line[j] == ')')) {
        return j + 1 < line.size() && line[j + 1] == ' ';
    }
    return false;
}

bool is_blank(std::string_view line) {
    return line.find_first_not_of(" \t\r") == std::string_view::npos;
}

// Track semantic context: are we inside a fenced code block or list?
// Also carries the HEADING BREADCRUMB (contextual retrieval): the stack of
// markdown headings enclosing the current line, e.g. {"Install","Linux"}.
struct ChunkContext {
    bool in_code_fence = false;
    int  list_indent   = -1;  // -1 = not in list; >= 0 = list item indent level
    // (level, text) pairs, outermost first. Level = number of '#'.
    std::vector<std::pair<int, std::string>> headings;
};

// Parse an ATX heading: returns its level (1-6) and stores the stripped
// title in `out`. 0 when the line is not a heading.
int parse_heading(std::string_view line, std::string& out) {
    std::size_t i = 0;
    while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) ++i;
    std::size_t h = i;
    while (h < line.size() && line[h] == '#') ++h;
    int level = static_cast<int>(h - i);
    if (level < 1 || level > 6) return 0;
    if (h < line.size() && line[h] != ' ' && line[h] != '\t') return 0;
    while (h < line.size() && (line[h] == ' ' || line[h] == '\t')) ++h;
    std::size_t e = line.size();
    while (e > h && (line[e-1] == ' ' || line[e-1] == '\t'
                     || line[e-1] == '#' || line[e-1] == '\r')) --e;
    out.assign(line.substr(h, e - h));
    return out.empty() ? 0 : level;
}

// Determine if this line is a safe break point given the context.
bool is_safe_break(std::string_view line, const ChunkContext& ctx) {
    // Never break inside a fenced code block.
    if (ctx.in_code_fence) return false;
    // Prefer breaking at blank lines, headings, or before new list items.
    if (is_blank(line)) return true;
    if (is_heading(line)) return true;
    // If we're in a list, only break before a new top-level item or heading.
    if (ctx.list_indent >= 0) {
        if (is_list_item(line)) {
            std::size_t indent = 0;
            while (indent < line.size() && (line[indent] == ' ' || line[indent] == '\t'))
                ++indent;
            // New list item at same or lower indent = safe break.
            return static_cast<int>(indent) <= ctx.list_indent;
        }
        return false;  // Continuation of current list item.
    }
    return false;
}

void update_context(std::string_view line, ChunkContext& ctx) {
    // Toggle code fence.
    if (is_fenced_code_start(line)) {
        ctx.in_code_fence = !ctx.in_code_fence;
        return;
    }
    if (ctx.in_code_fence) return;  // Inside code, don't track list.

    // Heading: pop deeper-or-equal levels, push this one (breadcrumb).
    {
        std::string title;
        if (int lvl = parse_heading(line, title); lvl > 0) {
            while (!ctx.headings.empty() && ctx.headings.back().first >= lvl)
                ctx.headings.pop_back();
            ctx.headings.emplace_back(lvl, std::move(title));
            ctx.list_indent = -1;
            return;
        }
    }

    // Track list indent.
    if (is_list_item(line)) {
        std::size_t indent = 0;
        while (indent < line.size() && (line[indent] == ' ' || line[indent] == '\t'))
            ++indent;
        ctx.list_indent = static_cast<int>(indent);
    } else if (is_blank(line) || is_heading(line)) {
        ctx.list_indent = -1;  // List ended.
    }
    // Non-blank, non-list, non-heading lines continue the current block.
}

// Situating string for a chunk: "path › H1 › H2 › H3" from the breadcrumb
// captured at the chunk's start. Capped so a pathological heading doesn't
// bloat every embedding input.
std::string breadcrumb_context(const std::string& path,
                               const ChunkContext& ctx) {
    std::string s = path;
    for (const auto& [lvl, title] : ctx.headings) {
        s += " \xe2\x80\xba ";   // ›
        s += title;
    }
    if (s.size() > 256) {
        std::size_t cut = 256;
        // Back off continuation bytes so the cap can't split a code point.
        while (cut > 0 && (static_cast<unsigned char>(s[cut]) & 0xC0) == 0x80)
            --cut;
        s.resize(cut);
    }
    return s;
}

} // namespace

std::vector<Chunk>
chunk_document(const std::string& path, const std::string& body,
               std::size_t max_lines, std::size_t max_chars,
               std::size_t overlap_lines) {
    // Split into lines, tracking 1-based line numbers.
    std::vector<std::string_view> lines;
    {
        std::size_t start = 0;
        for (std::size_t i = 0; i <= body.size(); ++i) {
            if (i == body.size() || body[i] == '\n') {
                lines.emplace_back(body.data() + start, i - start);
                start = i + 1;
            }
        }
    }

    // HARD-SPLIT pathologically long single lines (minified JS/CSS, a giant
    // one-line JSON blob, a base64 data URI) BEFORE chunking. Otherwise the
    // chunk loop always takes the first line whole — `taken == 0` bypasses
    // every overflow guard — so one 50KB line became one 50KB chunk, blowing
    // max_chars by 30×: wasted embed tokens and, if retrieved, a giant blob
    // dumped into the model's window. Split at UTF-8 boundaries so no piece
    // exceeds max_chars and no multibyte codepoint is cut. Pieces stay views
    // into `body`. Guard against a zero cap.
    if (max_chars > 4) {
        std::vector<std::string_view> split_lines;
        split_lines.reserve(lines.size());
        for (std::string_view ln : lines) {
            if (ln.size() <= max_chars) { split_lines.push_back(ln); continue; }
            std::size_t off = 0;
            while (ln.size() - off > max_chars) {
                std::size_t cut = off + max_chars;
                // Walk back off a UTF-8 continuation byte (0b10xxxxxx) so we
                // never split a multibyte codepoint. Bounded to 3 steps (max
                // UTF-8 sequence length after the lead byte).
                std::size_t guard = 0;
                while (cut > off + 1 && guard < 4 &&
                       (static_cast<unsigned char>(ln[cut]) & 0xC0) == 0x80) {
                    --cut; ++guard;
                }
                split_lines.emplace_back(ln.data() + off, cut - off);
                off = cut;
            }
            if (off < ln.size())
                split_lines.emplace_back(ln.data() + off, ln.size() - off);
        }
        lines.swap(split_lines);
    }

    std::vector<Chunk> out;
    std::size_t i = 0;
    const std::size_t n = lines.size();
    ChunkContext ctx;

    while (i < n) {
        std::size_t begin = i;
        std::size_t char_count = 0;
        std::size_t taken = 0;
        ChunkContext chunk_ctx = ctx;  // Context at chunk start.

        // Grow the chunk until a size bound is hit. Prefer to stop at safe
        // semantic boundaries (blank lines, headings, list transitions).
        while (i < n) {
            std::size_t llen = lines[i].size() + 1;  // +1 for the newline
            // A line-count overflow is a SOFT bound: never split mid-fence on
            // it, or a fenced code block straddling the boundary lands in no
            // single chunk (the closing ``` ends up in a later chunk). Keep
            // growing until the fence closes. char_count is the HARD cap that
            // still bounds a pathologically long unterminated fence.
            bool line_overflow = (taken >= max_lines);
            bool char_overflow = (char_count + llen > max_chars);
            bool would_overflow =
                (line_overflow && !chunk_ctx.in_code_fence) || char_overflow;

            if (would_overflow && taken > 0) {
                // Try to find a safe break point within the last few lines.
                // If we can't, break here anyway.
                break;
            }

            // Check for semantic break before this line (if we have content).
            if (taken > 0 && is_safe_break(lines[i], chunk_ctx)) {
                // Only break here if we're not in the middle of something.
                if (!chunk_ctx.in_code_fence) break;
            }

            update_context(lines[i], chunk_ctx);
            char_count += llen;
            ++taken;
            ++i;

            // Same soft/hard split discipline for the post-take bounds: a
            // line-count cap must not cut an open fence; char_count still can.
            if (taken >= max_lines && !chunk_ctx.in_code_fence) break;
            if (char_count >= max_chars) break;
        }

        std::size_t end = i;  // exclusive

        // Assemble the chunk text.
        std::string text;
        text.reserve(char_count);
        for (std::size_t j = begin; j < end; ++j) {
            text.append(lines[j].data(), lines[j].size());
            text.push_back('\n');
        }
        // Skip whitespace-only chunks.
        bool nonblank = text.find_first_not_of(" \t\r\n") != std::string::npos;
        if (nonblank) {
            Chunk c;
            c.path = path;
            c.line_start = static_cast<int>(begin + 1);
            c.line_end   = static_cast<int>(end);
            c.text = std::move(text);
            // Contextual retrieval: situate the chunk with the heading
            // breadcrumb in force at its START (chunk_ctx snapshot).
            c.context = breadcrumb_context(path, chunk_ctx);
            out.push_back(std::move(c));
        }

        // Overlap: step back a few lines so a boundary-spanning fact lands
        // in both chunks. Always make forward progress.
        std::size_t next;
        if (end < n && overlap_lines > 0 && end > begin + overlap_lines) {
            next = end - overlap_lines;
        } else {
            next = end;
        }
        if (next <= begin) next = begin + 1;

        // Advance the global context to the NEXT chunk's true start (`next`),
        // NOT to `end`. With overlap, `next < end`: re-running update_context
        // over the overlapped lines would double-toggle code fences / list
        // state and start the following chunk in a corrupted context.
        for (std::size_t j = begin; j < next && j < end; ++j)
            update_context(lines[j], ctx);

        i = next;
    }

    return out;
}

} // namespace agentty::rag
