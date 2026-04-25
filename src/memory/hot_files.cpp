#include "moha/memory/hot_files.hpp"

#include "moha/tool/util/fs_helpers.hpp"
#include "moha/tool/util/subprocess.hpp"

#include <algorithm>
#include <chrono>
#include <set>
#include <sstream>
#include <string>
#include <system_error>
#include <unordered_map>
#include <utility>

namespace moha::memory {

namespace fs = std::filesystem;

namespace {

constexpr auto kCacheTtl = std::chrono::seconds{60};

[[nodiscard]] bool is_skipped_dir(std::string_view name) {
    return name == ".git" || name == "node_modules" || name == "build"
        || name == "target" || name == "vendor" || name == "dist"
        || name == "out" || name == ".next" || name == ".moha"
        || name == "__pycache__" || name == ".cache";
}

[[nodiscard]] std::string fmt_age(std::chrono::system_clock::duration d) {
    auto s = std::chrono::duration_cast<std::chrono::seconds>(d).count();
    if (s < 60)         return std::to_string(s) + "s";
    if (s < 3600)       return std::to_string(s / 60) + "m";
    if (s < 86400)      return std::to_string(s / 3600) + "h";
    return std::to_string(s / 86400) + "d";
}

} // namespace

void HotFiles::set_workspace(const fs::path& workspace) {
    std::lock_guard lk(mu_);
    fs::path canon;
    std::error_code ec;
    canon = fs::weakly_canonical(workspace, ec);
    if (ec) canon = workspace;
    if (canon == workspace_) return;
    workspace_ = std::move(canon);
    cache_.clear();
    cache_at_ = {};
}

void HotFiles::invalidate() {
    std::lock_guard lk(mu_);
    cache_.clear();
    cache_at_ = {};
}

bool HotFiles::ready() const noexcept {
    std::lock_guard lk(mu_);
    return !workspace_.empty();
}

std::string HotFiles::compose_block(std::size_t max_bytes) const {
    std::lock_guard lk(mu_);
    auto now = std::chrono::steady_clock::now();
    if (!cache_.empty() && now - cache_at_ < kCacheTtl)
        return cache_;
    rebuild_cache_locked_(max_bytes);
    return cache_;
}

void HotFiles::rebuild_cache_locked_(std::size_t max_bytes) const {
    cache_.clear();
    cache_at_ = std::chrono::steady_clock::now();
    if (workspace_.empty()) return;

    // Three time buckets, oldest-cap = 7 days.
    auto now_sys = std::chrono::system_clock::now();

    // Combine git log + mtime fallback. git log is canonical;
    // mtime catches uncommitted local edits.
    auto from_git  = git_log_since_(std::chrono::hours{24 * 7});
    auto from_mt   = mtime_scan_since_(std::chrono::hours{24 * 7});

    // Merge by path, keeping the newer timestamp.
    std::unordered_map<std::string, std::chrono::system_clock::time_point> by_path;
    for (auto& e : from_git) {
        auto& slot = by_path[e.path];
        if (slot < e.modified_at) slot = e.modified_at;
    }
    for (auto& e : from_mt) {
        auto& slot = by_path[e.path];
        if (slot < e.modified_at) slot = e.modified_at;
    }

    if (by_path.empty()) return;

    // Bucket entries by recency.
    std::vector<HotEntry> last_hour, last_day, last_week;
    for (auto& [path, when] : by_path) {
        auto age = now_sys - when;
        if (age < std::chrono::hours{1})       last_hour.push_back({path, when});
        else if (age < std::chrono::hours{24}) last_day.push_back({path, when});
        else                                    last_week.push_back({path, when});
    }
    auto sort_recent = [](std::vector<HotEntry>& v) {
        std::ranges::sort(v, [](const HotEntry& a, const HotEntry& b) {
            return a.modified_at > b.modified_at;
        });
    };
    sort_recent(last_hour);
    sort_recent(last_day);
    sort_recent(last_week);

    std::ostringstream out;
    out << "# Files the user has been actively touching. Focus your\n"
           "# attention here unless the question is clearly about\n"
           "# something else.\n";
    auto write_bucket = [&](std::string_view label,
                            std::vector<HotEntry>& v,
                            std::size_t cap_count) {
        if (v.empty()) return;
        out << "\n## " << label << " (" << v.size()
            << (v.size() == 1 ? " file" : " files") << ")\n";
        std::size_t shown = 0;
        for (const auto& e : v) {
            if (shown >= cap_count) {
                out << "  ... +" << (v.size() - shown) << " more\n";
                break;
            }
            out << "  " << e.path
                << "  (" << fmt_age(now_sys - e.modified_at) << " ago)\n";
            ++shown;
        }
    };
    write_bucket("Last 60 min", last_hour, 12);
    write_bucket("Last 24 hr",  last_day,  10);
    write_bucket("Last 7 days", last_week,  6);

    cache_ = out.str();
    if (cache_.size() > max_bytes) {
        // Truncate to the last newline before the cap so we don't
        // half-emit a row.
        auto cut = cache_.rfind('\n', max_bytes);
        if (cut == std::string::npos) cut = max_bytes;
        cache_.resize(cut);
        cache_ += "\n[... truncated to fit budget]\n";
    }
}

std::vector<HotEntry>
HotFiles::git_log_since_(std::chrono::seconds since) const {
    // Fast path: ask git for a flat list of files touched in the
    // window, with the last-touched timestamp. We use --name-only
    // with a tiny pretty-format that lets us stitch path → time.
    std::vector<HotEntry> out;
    if (workspace_.empty()) return out;

    std::error_code ec;
    if (!fs::exists(workspace_ / ".git", ec)) return out;

    auto secs = std::to_string(since.count());
    auto r = tools::util::run_argv_s(
        std::vector<std::string>{
            "git", "-C", workspace_.string(),
            "log",
            "--since=" + secs + " seconds ago",
            "--name-only",
            "--pretty=format:%x01%ct"
        },
        /*max_bytes=*/256 * 1024,
        std::chrono::seconds{8});
    if (!r.started || r.exit_code != 0) return out;

    // Format: blocks separated by \x01<unixtime>\n then file paths
    // one per line until the next \x01.
    std::unordered_map<std::string, std::int64_t> latest;
    std::istringstream iss(r.output);
    std::string line;
    std::int64_t cur_t = 0;
    while (std::getline(iss, line)) {
        if (line.empty()) continue;
        if (line[0] == '\x01') {
            try { cur_t = std::stoll(line.substr(1)); } catch (...) { cur_t = 0; }
            continue;
        }
        auto& slot = latest[line];
        if (slot < cur_t) slot = cur_t;
    }
    out.reserve(latest.size());
    for (auto& [p, t] : latest) {
        // Skip files that no longer exist (deleted commits).
        std::error_code fec;
        if (!fs::exists(workspace_ / p, fec)) continue;
        out.push_back({p, std::chrono::system_clock::from_time_t(t)});
    }
    return out;
}

std::vector<HotEntry>
HotFiles::mtime_scan_since_(std::chrono::seconds since) const {
    std::vector<HotEntry> out;
    if (workspace_.empty()) return out;
    std::error_code ec;
    auto cutoff = std::chrono::system_clock::now() - since;
    auto cutoff_secs = std::chrono::duration_cast<std::chrono::seconds>(
        cutoff.time_since_epoch()).count();

    int budget = 5000;     // hard cap on entries scanned
    for (auto it = fs::recursive_directory_iterator(workspace_,
            fs::directory_options::skip_permission_denied, ec);
         it != fs::recursive_directory_iterator() && budget-- > 0;
         it.increment(ec))
    {
        if (ec) { ec.clear(); continue; }
        const auto& entry = *it;
        auto fn = entry.path().filename().string();
        if (entry.is_directory(ec)) {
            if (is_skipped_dir(fn)) it.disable_recursion_pending();
            continue;
        }
        if (!entry.is_regular_file(ec)) continue;
        if (fn.starts_with(".")) continue;
        auto mt = fs::last_write_time(entry.path(), ec);
        if (ec) { ec.clear(); continue; }
        auto mt_secs = std::chrono::duration_cast<std::chrono::seconds>(
            mt.time_since_epoch()).count();
        if (mt_secs < cutoff_secs) continue;
        auto rel = fs::relative(entry.path(), workspace_, ec).generic_string();
        if (ec || rel.empty()) continue;
        out.push_back({std::move(rel),
                       std::chrono::system_clock::from_time_t(mt_secs)});
    }
    return out;
}

HotFiles& shared_hot_files() {
    static HotFiles g;
    return g;
}

} // namespace moha::memory
