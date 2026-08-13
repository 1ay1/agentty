// decomposition_memory.cpp — retrieval-augmented orchestration store.
//
// Append-only JSONL of successful turn decompositions under
// <project-root>/.agentty/decompositions.jsonl. Retrieval is deliberately
// simple: exact turn-signature match first, then same-complexity-tier
// fallback, newest-first. The value is grounding the orchestrator in what
// actually worked in THIS repo, not a fancy similarity model.

#include "agentty/domain/decomposition_memory.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <vector>

#include <nlohmann/json.hpp>

#include "agentty/tool/util/fs_helpers.hpp"
#include "agentty/auth/auth.hpp"   // CrossProcessFileLock

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace agentty::smart {

struct DecompositionMemory::Impl {
    std::mutex mu;
    bool loaded = false;
    std::string loaded_for;
    std::string forced_root;
    std::vector<Decomposition> recs;   // chronological (append order)
    static constexpr std::size_t kMax = 400;   // cap the in-memory/scan size
    std::size_t disk_lines = 0;                 // append-only lines on disk

    std::string root() {
        if (!forced_root.empty()) return forced_root;
        // Active project dir (cwd clamped inside the workspace boundary),
        // not raw current_path — keeps per-workspace learning aligned with
        // the rest of persistence under --workspace.
        return tools::util::project_root().string();
    }
    fs::path path() {
        auto r = root();
        if (r.empty()) return {};
        return fs::path{r} / ".agentty" / "decompositions.jsonl";
    }

    void ensure_loaded() {
        std::string r = root();
        if (loaded && loaded_for == r) return;
        recs.clear();
        loaded = true;
        loaded_for = r;
        auto p = path();
        if (p.empty()) return;
        std::ifstream f(p);
        if (!f) return;
        std::string line;
        disk_lines = 0;
        while (std::getline(f, line)) {
            if (line.empty()) continue;
            ++disk_lines;
            try {
                auto j = json::parse(line);
                Decomposition d;
                d.signature = j.value("sig", "");
                d.gist      = j.value("gist", "");
                if (j.contains("steps") && j["steps"].is_array())
                    for (const auto& s : j["steps"])
                        if (s.is_string()) d.steps.push_back(s.get<std::string>());
                if (!d.signature.empty() && !d.steps.empty())
                    recs.push_back(std::move(d));
            } catch (...) { /* skip malformed line */ }
        }
        // Keep only the newest kMax so scans stay bounded.
        if (recs.size() > kMax)
            recs.erase(recs.begin(), recs.end() - static_cast<std::ptrdiff_t>(kMax));
    }

    // Rewrite the .jsonl from the newest kMax records ON DISK (re-read under
    // the lock so a peer process's appended records are merged in, not dropped)
    // when the append-only file has grown past the cap. Best-effort, atomic via
    // temp+rename; a failure leaves the original intact. Caller holds the lock.
    void compact_locked(const fs::path& p) {
        if (p.empty()) return;
        // Re-parse the current file so peer appends survive the rewrite.
        std::vector<Decomposition> disk;
        if (std::ifstream f{p}) {
            std::string line;
            while (std::getline(f, line)) {
                if (line.empty()) continue;
                try {
                    auto j = json::parse(line);
                    Decomposition d;
                    d.signature = j.value("sig", "");
                    d.gist      = j.value("gist", "");
                    if (j.contains("steps") && j["steps"].is_array())
                        for (const auto& s : j["steps"])
                            if (s.is_string()) d.steps.push_back(s.get<std::string>());
                    if (!d.signature.empty() && !d.steps.empty())
                        disk.push_back(std::move(d));
                } catch (...) { /* skip malformed */ }
            }
        }
        if (disk.size() > kMax)
            disk.erase(disk.begin(), disk.end() - static_cast<std::ptrdiff_t>(kMax));
        auto tmp = p; tmp += ".tmp";
        {
            std::ofstream f(tmp, std::ios::trunc);
            if (!f) return;
            for (const auto& d : disk) {
                json j;
                j["sig"]   = d.signature;
                j["gist"]  = d.gist;
                j["steps"] = d.steps;
                f << j.dump() << '\n';
            }
            if (!f) return;
            disk_lines = disk.size();
        }
        std::error_code ec;
        fs::rename(tmp, p, ec);
        if (ec) { fs::remove(tmp, ec); return; }
        recs = std::move(disk);   // adopt the merged view
    }
};

DecompositionMemory& DecompositionMemory::instance() {
    static DecompositionMemory s;
    return s;
}
DecompositionMemory::Impl& DecompositionMemory::impl() {
    static Impl i;
    return i;
}

static std::string tier_of(const std::string& sig) {
    auto c = sig.find(':');
    return c == std::string::npos ? sig : sig.substr(0, c);
}

void DecompositionMemory::record(const std::string& signature,
                                 std::string_view gist,
                                 std::vector<std::string> steps) {
    if (signature.empty() || steps.empty()) return;
    auto& d = impl();
    std::lock_guard<std::mutex> lk(d.mu);
    d.ensure_loaded();
    // Dedup: if the most recent record for this signature has identical steps,
    // skip (a repeated identical pattern adds no information).
    for (auto it = d.recs.rbegin(); it != d.recs.rend(); ++it) {
        if (it->signature == signature) {
            if (it->steps == steps) return;
            break;   // most recent for this sig differs → record it
        }
    }
    Decomposition rec{signature, std::string{gist}, steps};
    d.recs.push_back(rec);

    auto p = d.path();
    if (p.empty()) return;
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    // Serialize the append + any compaction against peer agentty processes
    // sharing this repo's store. Advisory + best-effort.
    auth::CrossProcessFileLock xlock(p);
    {
        std::ofstream f(p, std::ios::app);
        if (!f) return;
        json j;
        j["ts"]   = std::chrono::duration_cast<std::chrono::seconds>(
                        std::chrono::system_clock::now().time_since_epoch()).count();
        j["sig"]  = signature;
        j["gist"] = rec.gist;
        j["steps"] = rec.steps;
        f << j.dump() << '\n';
    }
    // Bound the in-memory view (record() pushes without the load-time cap) and
    // rewrite the file once it has drifted well past the cap.
    if (d.recs.size() > Impl::kMax)
        d.recs.erase(d.recs.begin(),
                     d.recs.end() - static_cast<std::ptrdiff_t>(Impl::kMax));
    if (++d.disk_lines > Impl::kMax * 4) d.compact_locked(p);
}

std::vector<Decomposition> DecompositionMemory::recall(const std::string& signature,
                                                       std::size_t k) {
    std::vector<Decomposition> out;
    if (signature.empty() || k == 0) return out;
    auto& d = impl();
    std::lock_guard<std::mutex> lk(d.mu);
    d.ensure_loaded();
    // Pass 1: exact signature, newest first.
    for (auto it = d.recs.rbegin(); it != d.recs.rend() && out.size() < k; ++it)
        if (it->signature == signature) out.push_back(*it);
    if (!out.empty()) return out;
    // Pass 2: same complexity tier, newest first.
    const std::string tier = tier_of(signature);
    for (auto it = d.recs.rbegin(); it != d.recs.rend() && out.size() < k; ++it)
        if (tier_of(it->signature) == tier) out.push_back(*it);
    return out;
}

std::size_t DecompositionMemory::learned_count() {
    auto& d = impl();
    std::lock_guard<std::mutex> lk(d.mu);
    d.ensure_loaded();
    return d.recs.size();
}

void DecompositionMemory::reset() {
    auto& d = impl();
    std::lock_guard<std::mutex> lk(d.mu);
    d.ensure_loaded();
    auto p = d.path();
    if (!p.empty()) { std::error_code ec; fs::remove(p, ec); }
    d.recs.clear();
}

void DecompositionMemory::set_root_for_test(std::string root) {
    auto& d = impl();
    std::lock_guard<std::mutex> lk(d.mu);
    d.forced_root = std::move(root);
    d.loaded = false;
    d.recs.clear();
}

} // namespace agentty::smart
