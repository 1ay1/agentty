#include "agentty/diff/diff.hpp"

#include <algorithm>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace agentty::diff {

namespace {
std::vector<std::string> split_lines(const std::string& s) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == '\n') { out.push_back(cur); cur.clear(); }
        else cur += c;
    }
    out.push_back(cur);
    return out;
}

// LCS-based diff, good enough for hunking.
struct Edit { enum K { Keep, Del, Ins } k; int a_idx, b_idx; };

// Worst-case DP table cap. Beyond this many cells the full O(N*M) table is
// both too slow and too memory-hungry, so the middle region falls back to a
// block replacement (delete-all-then-insert-all). The hunk still reconstructs
// `after` exactly; it just isn't minimal. 6M cells ~= 24MB of int, ~ms-scale.
constexpr std::size_t kCellCap = 6'000'000;

// LCS edits for the interned ranges a[a0,a1) vs b[b0,b1), appended to `out`
// in forward order. `ai`/`bi` are line ids (cheap int compares).
void lcs_middle(const std::vector<int>& ai, const std::vector<int>& bi,
                int a0, int a1, int b0, int b1, std::vector<Edit>& out) {
    int nn = a1 - a0, mm = b1 - b0;
    if (nn == 0) {
        for (int j = b0; j < b1; ++j) out.push_back({Edit::Ins, -1, j});
        return;
    }
    if (mm == 0) {
        for (int i = a0; i < a1; ++i) out.push_back({Edit::Del, i, -1});
        return;
    }
    if ((std::size_t)nn * (std::size_t)mm > kCellCap) {
        for (int i = a0; i < a1; ++i) out.push_back({Edit::Del, i, -1});
        for (int j = b0; j < b1; ++j) out.push_back({Edit::Ins, -1, j});
        return;
    }
    const int stride = mm + 1;
    std::vector<int> dp((std::size_t)(nn + 1) * (std::size_t)stride, 0);
    for (int i = 1; i <= nn; ++i) {
        const int av = ai[a0 + i - 1];
        int* row = &dp[(std::size_t)i * stride];
        const int* prev = &dp[(std::size_t)(i - 1) * stride];
        for (int j = 1; j <= mm; ++j)
            row[j] = (av == bi[b0 + j - 1]) ? prev[j - 1] + 1
                                            : std::max(prev[j], row[j - 1]);
    }
    std::vector<Edit> rev;
    int i = nn, j = mm;
    while (i > 0 && j > 0) {
        if (ai[a0 + i - 1] == bi[b0 + j - 1]) {
            rev.push_back({Edit::Keep, a0 + i - 1, b0 + j - 1}); --i; --j;
        } else if (dp[(std::size_t)(i - 1) * stride + j] >=
                   dp[(std::size_t)i * stride + (j - 1)]) {
            rev.push_back({Edit::Del, a0 + i - 1, -1}); --i;
        } else {
            rev.push_back({Edit::Ins, -1, b0 + j - 1}); --j;
        }
    }
    while (i > 0) { rev.push_back({Edit::Del, a0 + (--i), -1}); }
    while (j > 0) { rev.push_back({Edit::Ins, -1, b0 + (--j)}); }
    out.insert(out.end(), rev.rbegin(), rev.rend());
}

std::vector<Edit> compute_edits(const std::vector<std::string>& a,
                                const std::vector<std::string>& b) {
    int n = (int)a.size(), m = (int)b.size();

    // Intern lines to ints so the DP and trims compare ids, not std::strings.
    std::unordered_map<std::string_view, int> ids;
    ids.reserve((std::size_t)(n + m) * 2);
    std::vector<int> ai(n), bi(m);
    int next_id = 0;
    for (int i = 0; i < n; ++i) {
        auto [it, inserted] = ids.emplace(std::string_view(a[i]), next_id);
        if (inserted) ++next_id;
        ai[i] = it->second;
    }
    for (int j = 0; j < m; ++j) {
        auto [it, inserted] = ids.emplace(std::string_view(b[j]), next_id);
        if (inserted) ++next_id;
        bi[j] = it->second;
    }

    std::vector<Edit> edits;
    edits.reserve((std::size_t)(n + m));

    // Common prefix: definitely in the LCS, emit as Keep and skip the DP.
    int p = 0;
    while (p < n && p < m && ai[p] == bi[p]) {
        edits.push_back({Edit::Keep, p, p});
        ++p;
    }
    // Common suffix: shrink both ends inward (also in the LCS).
    int sa = n, sb = m;
    while (sa > p && sb > p && ai[sa - 1] == bi[sb - 1]) { --sa; --sb; }

    // Diff only the divergent middle, then re-attach the trimmed suffix.
    lcs_middle(ai, bi, p, sa, p, sb, edits);
    for (int k = 0; sa + k < n; ++k)
        edits.push_back({Edit::Keep, sa + k, sb + k});
    return edits;
}
} // namespace

FileChange compute(const std::string& path,
                   const std::string& before,
                   const std::string& after) {
    FileChange c;
    c.path = path;
    c.original_contents = before;
    c.new_contents = after;

    auto a = split_lines(before);
    auto b = split_lines(after);
    auto edits = compute_edits(a, b);

    const int ctx = 3;
    int added = 0, removed = 0;
    std::vector<bool> is_change(edits.size(), false);
    for (size_t k = 0; k < edits.size(); ++k)
        if (edits[k].k != Edit::Keep) is_change[k] = true;

    size_t k = 0;
    while (k < edits.size()) {
        while (k < edits.size() && !is_change[k]) ++k;
        if (k >= edits.size()) break;
        size_t start = (k > (size_t)ctx) ? k - ctx : 0;
        size_t end = k;
        while (end < edits.size()) {
            size_t last_change = end;
            size_t probe = end;
            size_t gap = 0;
            while (probe < edits.size() && gap <= (size_t)(2 * ctx)) {
                if (is_change[probe]) { last_change = probe; gap = 0; }
                else gap++;
                probe++;
            }
            if (last_change == end) break;
            end = last_change;
        }
        end = std::min(edits.size() - 1, end + ctx);

        Hunk h;
        int old_start = -1, new_start = -1;
        int old_len = 0, new_len = 0;
        std::ostringstream patch;
        // Emit deletions before insertions within each contiguous change run
        // (git convention). The LCS backtrace can group inserts ahead of
        // deletes; rendered through a two-column diff gutter that ordering
        // makes the old/new line numbers read out of sequence (new line 2
        // appearing above old line 2). Buffer each run and flush "-" before
        // "+" on the next context line so both gutter columns stay monotonic
        // and the changed lines line up.
        std::string del_buf, ins_buf;
        auto flush_run = [&] {
            if (!del_buf.empty()) patch << del_buf;
            if (!ins_buf.empty()) patch << ins_buf;
            del_buf.clear();
            ins_buf.clear();
        };
        for (size_t i2 = start; i2 <= end; ++i2) {
            const auto& e = edits[i2];
            if (e.k == Edit::Keep) {
                flush_run();
                if (old_start < 0) old_start = e.a_idx + 1;
                if (new_start < 0) new_start = e.b_idx + 1;
                old_len++; new_len++;
                patch << " " << a[e.a_idx] << "\n";
            } else if (e.k == Edit::Del) {
                if (old_start < 0) old_start = e.a_idx + 1;
                old_len++;
                del_buf += "-"; del_buf += a[e.a_idx]; del_buf += "\n";
                removed++;
            } else {
                if (new_start < 0) new_start = e.b_idx + 1;
                new_len++;
                ins_buf += "+"; ins_buf += b[e.b_idx]; ins_buf += "\n";
                added++;
            }
        }
        flush_run();
        h.old_start = std::max(1, old_start);
        h.new_start = std::max(1, new_start);
        h.old_len = old_len;
        h.new_len = new_len;
        h.patch = patch.str();
        c.hunks.push_back(std::move(h));
        k = end + 1;
    }

    c.added = added;
    c.removed = removed;
    return c;
}

std::string render_unified(const FileChange& c) {
    std::ostringstream oss;
    oss << "--- a/" << c.path << "\n";
    oss << "+++ b/" << c.path << "\n";
    for (const auto& h : c.hunks) {
        oss << "@@ -" << h.old_start << "," << h.old_len
            << " +" << h.new_start << "," << h.new_len << " @@\n";
        oss << h.patch;
    }
    return oss.str();
}

std::string apply_accepted(const FileChange& c) {
    // If all hunks accepted -> return new_contents. Otherwise rebuild by
    // iterating hunks and choosing which side to emit.
    bool all_accepted = !c.hunks.empty() &&
        std::all_of(c.hunks.begin(), c.hunks.end(),
                    [](const Hunk& h){ return h.status == Hunk::Status::Accepted; });
    if (all_accepted) return c.new_contents;
    bool none_accepted = std::all_of(c.hunks.begin(), c.hunks.end(),
        [](const Hunk& h){ return h.status != Hunk::Status::Accepted; });
    if (none_accepted) return c.original_contents;

    // Partial acceptance: reconstruct from original, applying only accepted hunks.
    auto orig_lines = split_lines(c.original_contents);
    std::vector<std::string> out;
    size_t orig_cursor = 0;
    for (const auto& h : c.hunks) {
        size_t h_start = (size_t)std::max(0, h.old_start - 1);
        while (orig_cursor < h_start && orig_cursor < orig_lines.size())
            out.push_back(orig_lines[orig_cursor++]);
        if (h.status == Hunk::Status::Accepted) {
            std::istringstream iss(h.patch);
            std::string line;
            while (std::getline(iss, line)) {
                if (line.empty()) continue;
                char tag = line[0];
                std::string body = line.substr(1);
                if (tag == ' ' || tag == '+') out.push_back(body);
            }
            orig_cursor = h_start + (size_t)h.old_len;
        } else {
            size_t take = (size_t)h.old_len;
            for (size_t i = 0; i < take && orig_cursor < orig_lines.size(); ++i)
                out.push_back(orig_lines[orig_cursor++]);
        }
    }
    while (orig_cursor < orig_lines.size())
        out.push_back(orig_lines[orig_cursor++]);

    std::string joined;
    for (size_t i = 0; i < out.size(); ++i) {
        joined += out[i];
        if (i + 1 < out.size()) joined += "\n";
    }
    return joined;
}

} // namespace agentty::diff
