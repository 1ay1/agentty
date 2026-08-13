// routing_memory.cpp — the per-workspace routing prior (see header).
//
// Beta-smoothed regret accounting per turn-signature, persisted append-only to
// <cwd>/.agentty/routing_memory.tsv, mirroring the RAG FeedbackStore. Each
// signature accrues `routed` (denominator) and a signed `regret` sum; the
// prior is the smoothed regret rate mapped to a bounded [-1,+1] effort-bias,
// scaled by evidence confidence. Best-effort: any failure ⇒ neutral.

#include "agentty/domain/routing_memory.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <unordered_map>

#include "agentty/tool/util/fs_helpers.hpp"
#include "agentty/auth/auth.hpp"   // CrossProcessFileLock

namespace fs = std::filesystem;

namespace agentty::smart {

std::string turn_signature(Complexity tier, std::string_view text) {
    // Cheap token-class buckets, order-independent, low cardinality.
    bool has_q = false, is_long = false, looks_code = false;
    std::size_t words = 0;
    bool in = false;
    for (char c : text) {
        const bool ws = (c == ' ' || c == '\t' || c == '\n' || c == '\r');
        if (!ws && !in) { ++words; in = true; }
        else if (ws) in = false;
        if (c == '?') has_q = true;
        // A crude "mentions code" signal: path separators, dotted idents,
        // camelCase, or an extension-like token.
        if (c == '/' || c == '.' || c == '_' || (c >= 'A' && c <= 'Z')) looks_code = true;
    }
    is_long = words >= 40;

    // Coarse INTENT axis: the leading verb clusters distinct kinds of work
    // (fix vs. add vs. explain) that otherwise collide onto one signature and
    // bleed each other's priors. Lowercased opening scan, single char code,
    // so cardinality stays bounded (4 values).
    char intent = '.';
    {
        std::string head;
        head.reserve(24);
        for (char c : text) {
            if (head.size() >= 24) break;
            head.push_back((c >= 'A' && c <= 'Z') ? char(c + 32) : c);
        }
        auto has = [&](std::string_view w){ return head.find(w) != std::string::npos; };
        if (has("fix") || has("bug") || has("broke") || has("error") || has("debug"))
            intent = 'f';                                   // repair
        else if (has("add") || has("implement") || has("create") || has("write") || has("build"))
            intent = 'a';                                   // build
        else if (has("why") || has("explain") || has("how") || has("what") || has("understand"))
            intent = 'e';                                   // explain
    }

    std::string sig{to_string(tier)};
    sig += has_q     ? ":q"  : ":.";
    sig += looks_code? ":c"  : ":.";
    sig += is_long   ? ":l"  : ":.";
    sig += ":"; sig += intent;
    return sig;
}

struct RoutingMemory::Impl {
    std::mutex mu;
    bool loaded = false;
    std::string loaded_for;
    std::string forced_root;   // test seam
    struct Tally { double routed = 0.0; double regret = 0.0; };
    std::unordered_map<std::string, Tally> counts;
    std::size_t disk_lines = 0;   // append-only lines currently on disk

    static constexpr double kMaxBias = 1.0;   // clamp of the returned prior
    static constexpr double kPriorN  = 5.0;   // routed count for ~half confidence
    // Append-only lines accumulate forever (a handful of low-cardinality
    // signatures, re-added every turn), and ensure_loaded replays the whole
    // file each launch. Rewrite from the aggregated in-memory state once the
    // file grows past this — bounded disk + bounded startup replay, matching
    // the header's "bounded output" contract.
    static constexpr std::size_t kCompactThreshold = 2000;

    fs::path tsv_path() {
        std::string root = forced_root;
        if (root.empty()) {
            // The ACTIVE PROJECT dir (process cwd clamped inside the access
            // boundary), NOT raw current_path — under --workspace / the two
            // differ and the store must land where the rest of persistence
            // does. Best-effort: empty ⇒ neutral / no persistence.
            auto pr = tools::util::project_root();
            if (pr.empty()) return {};
            root = pr.string();
        }
        return fs::path{root} / ".agentty" / "routing_memory.tsv";
    }

    // Parse an append-only TSV into `into`, returning the physical line count.
    // Shared by ensure_loaded and the compaction re-read (so a compaction sees
    // any lines a PEER process appended and never drops them).
    static std::size_t parse_into(std::unordered_map<std::string, Tally>& into,
                                  const fs::path& p) {
        std::ifstream f(p);
        if (!f) return 0;
        std::size_t n = 0;
        std::string line;
        while (std::getline(f, line)) {
            ++n;
            // <epoch>\t<routed|regret>\t<signature>\t<delta>
            auto t1 = line.find('\t');
            if (t1 == std::string::npos) continue;
            auto t2 = line.find('\t', t1 + 1);
            if (t2 == std::string::npos) continue;
            auto t3 = line.find('\t', t2 + 1);
            std::string kind = line.substr(t1 + 1, t2 - t1 - 1);
            std::string sig  = (t3 == std::string::npos)
                                 ? line.substr(t2 + 1)
                                 : line.substr(t2 + 1, t3 - t2 - 1);
            if (sig.empty()) continue;
            if (kind == "routed") into[sig].routed += 1.0;
            else if (kind == "regret" && t3 != std::string::npos) {
                try { into[sig].regret += std::stod(line.substr(t3 + 1)); }
                catch (...) {}
            }
        }
        return n;
    }

    void ensure_loaded() {
        std::string root = forced_root;
        if (root.empty())
            root = tools::util::project_root().string();
        if (loaded && loaded_for == root) return;
        counts.clear();
        loaded = true;
        loaded_for = root;
        auto p = tsv_path();
        if (p.empty()) return;
        disk_lines = parse_into(counts, p);
    }

    void append(const char* kind, const std::string& sig, double delta) {
        auto p = tsv_path();
        if (p.empty()) return;
        std::error_code ec;
        fs::create_directories(p.parent_path(), ec);
        // Serialize append+compact across separate agentty processes sharing
        // this repo's store, so two instances can't interleave a partial
        // append with a compaction rewrite. Advisory + best-effort: if the
        // lock can't be taken we proceed anyway (O_APPEND writes stay atomic).
        auth::CrossProcessFileLock xlock(p);
        {
            std::ofstream f(p, std::ios::app);
            if (!f) return;
            auto now = std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
            f << now << '\t' << kind << '\t' << sig << '\t' << delta << '\n';
        }
        if (++disk_lines > kCompactThreshold) compact_locked(p);
    }

    // Rewrite the file from the aggregated tallies: at most two lines (routed +
    // regret) per distinct signature, collapsing thousands of redundant append
    // lines. Re-reads the on-disk file FIRST under the lock so any lines a peer
    // process appended (that this process never loaded) are merged in, never
    // dropped. Atomic via temp+rename; a failure leaves the append-only file
    // intact. Caller must already hold the cross-process lock on `p`.
    void compact_locked(const fs::path& p) {
        if (p.empty()) return;
        // Merge peer writes: start from a fresh parse of the current file, not
        // this process's possibly-stale in-memory view.
        std::unordered_map<std::string, Tally> merged;
        parse_into(merged, p);
        auto tmp = p; tmp += ".tmp";
        {
            std::ofstream f(tmp, std::ios::trunc);
            if (!f) return;
            auto now = std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
            std::size_t n = 0;
            for (const auto& [sig, t] : merged) {
                if (t.routed != 0.0) { f << now << "\trouted\t" << sig << '\t' << t.routed << '\n'; ++n; }
                if (t.regret != 0.0) { f << now << "\tregret\t" << sig << '\t' << t.regret << '\n'; ++n; }
            }
            if (!f) return;
            disk_lines = n;
        }
        std::error_code ec;
        fs::rename(tmp, p, ec);
        if (ec) { fs::remove(tmp, ec); return; }   // keep the original on failure
        // Adopt the merged view so this process's prior reflects peer evidence.
        counts = std::move(merged);
    }
};

RoutingMemory& RoutingMemory::instance() {
    static RoutingMemory s;
    return s;
}

RoutingMemory::Impl& RoutingMemory::impl() {
    static Impl i;
    return i;
}

int RoutingMemory::prior_bias(const std::string& signature) {
    auto& d = impl();
    std::lock_guard<std::mutex> lk(d.mu);
    d.ensure_loaded();
    auto it = d.counts.find(signature);
    if (it == d.counts.end() || it->second.routed <= 0.0) return 0;
    const double routed = it->second.routed;
    const double regret = it->second.regret;
    // Mean signed regret per routed turn, in ~[-1, 1]. A positive mean means
    // this signature has historically needed MORE effort than the heuristic
    // gave it.
    const double rate = regret / routed;
    // Confidence grows with sample count (saturating), so a single event never
    // swings the prior.
    const double conf = routed / (routed + Impl::kPriorN);
    double bias = rate * conf * Impl::kMaxBias;
    bias = std::clamp(bias, -Impl::kMaxBias, Impl::kMaxBias);
    // Return a whole effort-step: only a confident, sustained signal rounds to
    // ±1; weak evidence stays neutral.
    if (bias >= 0.5)  return 1;
    if (bias <= -0.5) return -1;
    return 0;
}

void RoutingMemory::note_routed(const std::string& signature) {
    if (signature.empty()) return;
    auto& d = impl();
    std::lock_guard<std::mutex> lk(d.mu);
    d.ensure_loaded();
    d.counts[signature].routed += 1.0;
    d.append("routed", signature, 1.0);
}

void RoutingMemory::note_regret(const std::string& signature, int direction) {
    if (signature.empty() || direction == 0) return;
    auto& d = impl();
    std::lock_guard<std::mutex> lk(d.mu);
    d.ensure_loaded();
    const double delta = direction > 0 ? 1.0 : -1.0;
    d.counts[signature].regret += delta;
    d.append("regret", signature, delta);
}

std::size_t RoutingMemory::learned_count() {
    auto& d = impl();
    std::lock_guard<std::mutex> lk(d.mu);
    d.ensure_loaded();
    return d.counts.size();
}

void RoutingMemory::reset() {
    auto& d = impl();
    std::lock_guard<std::mutex> lk(d.mu);
    d.ensure_loaded();
    auto p = d.tsv_path();
    if (!p.empty()) { std::error_code ec; fs::remove(p, ec); }
    d.counts.clear();
}

void RoutingMemory::set_root_for_test(std::string root) {
    auto& d = impl();
    std::lock_guard<std::mutex> lk(d.mu);
    d.forced_root = std::move(root);
    d.loaded = false;
    d.counts.clear();
}

} // namespace agentty::smart
