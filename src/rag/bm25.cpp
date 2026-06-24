// agentty::rag — BM25, RRF, cosine, and the document chunker.
// Pure C++/STL; no third-party deps. See rag.hpp for the design rationale.

#include "agentty/rag/rag.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <unordered_map>

namespace agentty::rag {

namespace {

// Lowercase alphanumeric-run tokenizer. No stemming, no stopword list —
// BM25's IDF term already down-weights ubiquitous tokens, and keeping the
// tokenizer trivial keeps indexing fast and predictable. Tokens shorter
// than 2 chars are dropped (single letters carry no retrieval signal).
void tokenize(std::string_view s, std::vector<std::string>& out) {
    std::string cur;
    cur.reserve(24);
    auto flush = [&] {
        if (cur.size() >= 2) out.push_back(cur);
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
    double dot = 0.0, na = 0.0, nb = 0.0;
    for (std::size_t i = 0; i < a.size(); ++i) {
        dot += static_cast<double>(a[i]) * b[i];
        na  += static_cast<double>(a[i]) * a[i];
        nb  += static_cast<double>(b[i]) * b[i];
    }
    if (na <= 0.0 || nb <= 0.0) return 0.0;
    return dot / (std::sqrt(na) * std::sqrt(nb));
}

std::vector<std::pair<std::uint32_t, double>>
reciprocal_rank_fusion(
    const std::vector<std::vector<std::uint32_t>>& ranked_lists,
    double k, std::size_t out_k) {
    std::unordered_map<std::uint32_t, double> fused;
    for (const auto& list : ranked_lists) {
        for (std::size_t rank = 0; rank < list.size(); ++rank) {
            // rank is 0-based; RRF uses 1-based rank.
            fused[list[rank]] += 1.0 / (k + static_cast<double>(rank + 1));
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
struct ChunkContext {
    bool in_code_fence = false;
    int  list_indent   = -1;  // -1 = not in list; >= 0 = list item indent level
};

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
            bool would_overflow =
                (taken >= max_lines) || (char_count + llen > max_chars);
            
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
            
            if (taken >= max_lines) break;
            if (char_count >= max_chars) break;
        }

        std::size_t end = i;  // exclusive
        // Update global context to the end of this chunk.
        for (std::size_t j = begin; j < end; ++j)
            update_context(lines[j], ctx);

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
            out.push_back(std::move(c));
        }

        // Overlap: step back a few lines so a boundary-spanning fact lands
        // in both chunks. Always make forward progress.
        if (end < n && overlap_lines > 0 && end > begin + overlap_lines) {
            i = end - overlap_lines;
        } else {
            i = end;
        }
        if (i <= begin) i = begin + 1;
    }

    return out;
}

} // namespace agentty::rag
