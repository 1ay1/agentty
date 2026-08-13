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
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string_view>
#include <unordered_map>

#include "agentty/tool/util/fs_helpers.hpp"
#include "agentty/auth/auth.hpp"   // CrossProcessFileLock
#include "agentty/domain/smart_tuning.hpp"

namespace fs = std::filesystem;

namespace agentty::smart {

namespace {

// FNV-1a over a byte range — cheap, well-mixed, deterministic across processes.
constexpr std::uint64_t fnv1a(std::string_view s, std::uint64_t h = 1469598103934665603ULL) noexcept {
    for (unsigned char c : s) { h ^= c; h *= 1099511628211ULL; }
    return h;
}

// The COARSE prefix: classified tier + language-agnostic STRUCTURAL buckets
// (question shape, code density, length). This is the low-cardinality key the
// fine signature backs off TO when a specific turn hasn't accrued evidence yet.
// No English verbs — structure only, so it generalises across languages.
std::string coarse_prefix(Complexity tier, std::string_view text) {
    bool has_q = false, looks_code = false;
    std::size_t glyphs = 0;
    bool in = false;
    for (unsigned char c : text) {
        const bool ws = (c == ' ' || c == '\t' || c == '\n' || c == '\r');
        if (!ws && !in) { in = true; }
        else if (ws) in = false;
        if ((c & 0xC0) != 0x80) ++glyphs;   // count glyphs, not continuation bytes
        if (c == '?') has_q = true;
        if (c == '/' || c == '_' || (c >= 'A' && c <= 'Z')) looks_code = true;
    }
    std::string p{to_string(tier)};
    p += has_q     ? ":q" : ":.";
    p += looks_code? ":c" : ":.";
    p += (glyphs >= 220) ? ":L" : (glyphs >= 60 ? ":m" : ":s");   // size band
    return p;
}

// The FINE discriminator: a bounded feature-hash of the SALIENT content tokens
// (order-independent set, so word order doesn't fragment the key). A token is a
// maximal run of letter/digit-ish bytes ≥ 3 glyphs (drops most stopwords and
// punctuation without an English stoplist — works for any script). We fold each
// token's FNV hash into a small bitset over a fixed modulus, then emit it as a
// short hex code. Distinct turns get distinct codes; the modulus caps
// cardinality so the store stays bounded, and collisions merely share a prior
// (harmless — same effect as a coarser bucket).
std::string content_hash(std::string_view text) {
    constexpr int kBuckets = 64;            // fine-key space per coarse prefix
    std::uint64_t bits = 0;
    std::string tok;
    auto flush = [&] {
        // ≥ 3 glyphs of content; hash lowercased.
        if (tok.size() >= 3) bits |= (1ULL << (fnv1a(tok) % kBuckets));
        tok.clear();
    };
    for (unsigned char c : text) {
        const bool wordish =
            (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')
            || (c >= 'A' && c <= 'Z') || (c & 0x80);   // keep multibyte scripts
        if (wordish) tok.push_back(static_cast<char>(
                         (c >= 'A' && c <= 'Z') ? c + 32 : c));
        else flush();
    }
    flush();
    // Emit the folded bitset as fixed-width hex (stable, tab/newline-free).
    char buf[17];
    std::snprintf(buf, sizeof buf, "%016llx", static_cast<unsigned long long>(bits));
    return std::string{buf};
}

} // namespace

std::string turn_signature(Complexity tier, std::string_view text) {
    // HIERARCHICAL key: "<coarse>#<fine>". prior_bias reads the fine key when it
    // has evidence and BACKS OFF to the coarse prefix when it doesn't — so a
    // brand-new specific turn borrows the prior of its structural class, and a
    // well-seen specific turn gets its own sharper prior. This replaces the old
    // 4-boolean × English-verb bucketing, which both collided unrelated turns
    // and couldn't see past an English lexicon.
    return coarse_prefix(tier, text) + "#" + content_hash(text);
}

// Split a hierarchical signature at '#'. Older/coarse-only signatures (no '#')
// are their own prefix with an empty fine part — fully backward compatible with
// any TSV rows written before the hierarchy landed.
static std::string_view sig_coarse(std::string_view sig) {
    auto h = sig.find('#');
    return h == std::string_view::npos ? sig : sig.substr(0, h);
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
    // Default evidence pseudo-count; the live value comes from
    // tuning::prior_evidence() (AGENTTY_SMART_PRIOR_EVIDENCE) which defaults
    // here. Kept as the documented baseline.
    static constexpr double kPriorN  = 5.0;
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

    // Hierarchical estimate. The signature is "<coarse>#<fine>":
    //   • FINE tally  = this exact turn's own evidence (sharp but often thin).
    //   • COARSE tally = the aggregate over EVERY fine key sharing this coarse
    //     structural prefix (blunt but well-populated) — the parent bucket.
    // We shrink the fine estimate toward the coarse one by evidence: a fine key
    // with little data borrows its structural class's prior; a well-seen fine
    // key trusts its own. This is the classic backoff/empirical-Bayes move — it
    // gives specificity when confident and generalization when not, which the
    // old flat single-bucket key could not.
    const std::string_view coarse = sig_coarse(signature);

    double f_routed = 0.0, f_regret = 0.0;   // fine (exact key)
    double c_routed = 0.0, c_regret = 0.0;   // coarse (all keys with this prefix)
    if (auto it = d.counts.find(signature); it != d.counts.end()) {
        f_routed = it->second.routed; f_regret = it->second.regret;
    }
    for (const auto& [sig, t] : d.counts)
        if (sig_coarse(sig) == coarse) { c_routed += t.routed; c_regret += t.regret; }
    if (c_routed <= 0.0) return 0;   // never seen this structural class at all

    const double f_rate = f_routed > 0.0 ? f_regret / f_routed : 0.0;
    const double c_rate = c_regret / c_routed;
    // Evidence pseudo-count: how much data before a prior is trusted. Env-
    // tunable (AGENTTY_SMART_PRIOR_EVIDENCE) — lower reacts faster, higher is
    // more conservative.
    const double prior_n = tuning::prior_evidence();
    // Shrinkage weight toward the fine estimate grows with its own sample count;
    // with zero fine evidence it's a pure coarse (backoff) estimate.
    const double alpha = f_routed / (f_routed + prior_n);
    const double rate  = alpha * f_rate + (1.0 - alpha) * c_rate;

    // Overall confidence from the TOTAL evidence backing the estimate (fine +
    // its coarse parent), so a thin fine key riding a rich parent is still
    // allowed to move the prior.
    const double evidence = std::max(f_routed, c_routed);
    const double conf = evidence / (evidence + prior_n);

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
