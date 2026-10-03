// The trust-handoff gate. See the header for why this sits on the tool call
// rather than inside the sandbox.

#include "agentty/tool/util/handoff_gate.hpp"

#include <jaal/kernel/guarded.hpp>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "agentty/tool/util/sandbox.hpp"
#include "agentty/util/logx.hpp"

namespace agentty::tools::util::handoff {

namespace fs = std::filesystem;

namespace {

// Same storage shape and the same reasoning as the broker's blocked feed: see
// sandbox_broker.cpp. `jaal::guarded` because access is only possible through
// `with()`, so "forgot the lock" is unrepresentable rather than a review item;
// function-local static because the first handoff can arrive from a tool worker
// during startup and a file-scope global would race its own constructor.
jaal::guarded<std::vector<sandbox_cfg::TrustHandoff>>& feed() {
    static jaal::guarded<std::vector<sandbox_cfg::TrustHandoff>> f;
    return f;
}

// Bounded for the same reason the broker's feed is: a loop writing to a trusted
// path would otherwise grow this without limit, which is a denial of service the
// gate inflicts on its own host. Smaller than the broker's 64 because a handoff
// is a far rarer and far louder event -- if there are 32 distinct ones, the
// count stopped being the information a while ago.
constexpr std::size_t kMaxHandoffs = 32;

[[nodiscard]] std::uint64_t now_ms() {
    using namespace std::chrono;
    return static_cast<std::uint64_t>(
        duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count());
}

void remember(const sandbox_cfg::TrustHandoff& h) {
    feed().with([](std::vector<sandbox_cfg::TrustHandoff>& all,
                   sandbox_cfg::TrustHandoff incoming) {
        // Collapse a repeat of the same path. Unlike the broker's feed this
        // checks the WHOLE list rather than just the back: a handoff is
        // identified by its path, and the same file rewritten twice in a turn
        // is one fact about one file, not a sequence worth preserving.
        for (auto& existing : all) {
            if (existing.write.path == incoming.write.path) {
                existing.write.at_ms = incoming.write.at_ms;
                existing.write.command = std::move(incoming.write.command);
                return;
            }
        }
        all.push_back(std::move(incoming));
        // Drop the oldest when full, keeping the newest -- flushing an early
        // handoff out of view costs an attacker 32 distinct ones, each of which
        // is itself recorded and (by default) refused.
        if (all.size() > kMaxHandoffs) all.erase(all.begin());
    }, h);
}

}  // namespace

Verdict check_with(std::string_view path, std::string_view tool,
                   sandbox_cfg::HandoffPolicy policy) {
    Verdict v;

    sandbox_cfg::TrustKind kind{};
    bool trusted = sandbox_cfg::is_host_trusted(path, &kind);

    // If the SPELLING is innocent, try the RESOLVED path before believing it.
    //
    // is_host_trusted() is a pure shape walk, so it sees only what it was
    // handed. A symlinked directory therefore hides a trusted shape entirely:
    //
    //     ln -s .git/hooks innocuous
    //     write innocuous/post-merge      <- installs a real git hook
    //
    // Measured: "innocuous/post-merge" reads as PLAIN, and the same file via
    // ".git/hooks/post-merge" reads as trusted. The write lands in the hook
    // directory either way, so matching only the spelling is a gate the model
    // can step around by creating one symlink -- which it is allowed to do,
    // since a symlink inside the workspace is ordinary work.
    //
    // Resolution is the second check rather than a replacement for the first:
    // weakly_canonical touches the filesystem, and the common case is an
    // ordinary path that must stay a few string compares. Only a path that
    // looks innocent pays for the resolve, and `trusted` short-circuits the
    // one that already matched.
    if (!trusted) {
        std::error_code ec;
        const fs::path resolved = fs::weakly_canonical(fs::path{path}, ec);
        if (!ec && !resolved.empty() && resolved != fs::path{path}) {
            sandbox_cfg::TrustKind rk{};
            if (sandbox_cfg::is_host_trusted(resolved.string(), &rk)) {
                trusted = true;
                kind = rk;
                AGT_LOG(Tool, Warn, "sandbox.handoff",
                        "path resolves into a trusted location: {} -> {}",
                        logx::body(std::string{path}),
                        logx::body(resolved.string()));
            }
        }
    }

    if (!trusted) {
        // The common case, and it must stay cheap: this runs on every write the
        // agent makes. `is_host_trusted` is a table walk over path shapes with
        // no I/O, so an ordinary path costs a few string compares.
        return v;
    }

    v.is_handoff = true;
    v.kind = kind;

    // Record BEFORE branching on the policy. A handoff that was allowed under
    // `Allow` is still the single most interesting thing that happened to the
    // workspace this turn, and a feed that only lists refusals would go silent
    // exactly when the user has turned the gate off -- which is when they most
    // need to see it.
    sandbox_cfg::TrustHandoff h;
    h.write.path = std::string{path};
    h.write.command = std::string{tool};
    h.write.at_ms = now_ms();
    h.kind = kind;
    remember(h);

    const auto why = sandbox_cfg::explain(kind);

    // Logged at every outcome, allow included, for the same reason the broker
    // logs allows: the trail is what makes the boundary teachable. The path is
    // behind logx::body() because it is workspace content.
    AGT_LOG(Tool, Info, "sandbox.handoff", "{} {} ({}) policy={}",
            policy == sandbox_cfg::HandoffPolicy::Refuse ? "REFUSED" : "allowed",
            logx::body(std::string{path}), why,
            sandbox_cfg::to_string(policy));

    switch (policy) {
        case sandbox_cfg::HandoffPolicy::Allow:
            return v;

        case sandbox_cfg::HandoffPolicy::Warn:
            // Proceeds, but says so. The reason rides along so the caller can
            // surface it without re-deriving the explanation.
            v.reason = std::string{"wrote "} + std::string{why};
            return v;

        case sandbox_cfg::HandoffPolicy::Refuse:
            break;
    }

    v.allowed = false;
    // Phrased for the MODEL as much as the user: a refusal that only says "no"
    // gets retried, and a retry loop on a security gate is worse than either
    // outcome. Naming the mechanism and the alternative ends the loop in one
    // turn.
    v.reason =
        "refused: " + std::string{why} + ". The sandbox confines this process, "
        "but a file the host later executes escapes it by being run outside. "
        "Write the content somewhere inert and ask the user to wire it up, or "
        "change Trust handoff in the Sandbox pane.";
    return v;
}

Verdict check(std::string_view path, std::string_view tool) {
    // `--sandbox off` turns this off as well.
    //
    // The gate is not a sandbox WALL (it lives on the tool call, §14) but it is
    // part of the same control, and a user who disabled sandboxing did not opt
    // into one surviving piece of it refusing their writes. "off" has to mean
    // off, or the flag is a lie in the other direction -- the same honesty rule
    // the rest of this subsystem is built on.
    //
    // Checked through requested_mode() rather than is_active(): a host with no
    // usable backend still gets the gate, because that is a host that FAILED to
    // sandbox rather than a user who asked not to be. Those are different
    // answers and conflating them would quietly drop the gate on exactly the
    // machines with the fewest other walls.
    if (sandbox::requested_mode() == sandbox::Mode::Off) return {};

    // Reads the SEALED config, so the policy cannot change under a running
    // turn -- the same guarantee every other wall here has.
    return check_with(path, tool, sandbox::config_snapshot()->handoff);
}

std::vector<sandbox_cfg::TrustHandoff> handoff_feed() {
    return feed().with(
        [](std::vector<sandbox_cfg::TrustHandoff>& all) { return all; });
}

void clear_handoff_feed() {
    feed().with([](std::vector<sandbox_cfg::TrustHandoff>& all) { all.clear(); });
}

// ── Detection for the shell path ──────────────────────────────────

namespace {

// Directories the snapshot will not descend into.
//
// Deliberately NOT the masking walk's kNeverWalk list, and the difference is
// the point: that list skips .git because it holds no secrets, while .git is
// one of the most important things to WATCH here (.git/config carries a
// credential helper, .git/hooks runs on the next commit). What is skipped here
// is only the bulk that cannot plausibly contain a trusted path we care about
// -- build output and dependency trees. node_modules is skipped despite
// `node_modules/.bin` being a trusted shape: walking it costs more than the
// detection is worth, and a .bin shim is reached through package.json, which
// IS watched.
constexpr std::string_view kSkipDirs[] = {
    "node_modules", "target", "build", "dist", "out", "__pycache__",
    ".mypy_cache", ".pytest_cache", ".cache", ".next", ".gradle", ".tox",
};

// Inside a .git directory, only these subtrees are worth watching.
//
// Measured on the agentty repo: without this, .git contributed 1605 of 1824
// "trusted" paths -- almost all of it the object store, packed refs and logs.
// The ShapeRule for .git is a COMPONENT match, so every blob under
// .git/objects/ reads as trusted, and none of it is: git executes hooks and
// honours config, it does not run its own object database. Watching 1605 inert
// files to catch writes to three is how a detector becomes noise, and noise is
// how a security feature gets ignored.
//
// Everything a host tool actually RUNS from .git lives in one of these.
constexpr std::string_view kGitWatch[] = {"hooks", "info"};

// Is `rel` inside a .git directory, and if so is it a part we care about?
[[nodiscard]] bool git_noise(const fs::path& p) {
    bool in_git = false;
    for (auto it = p.begin(); it != p.end(); ++it) {
        if (*it != ".git") continue;
        in_git = true;
        auto next = std::next(it);
        if (next == p.end()) return false;           // ".git" itself
        const std::string seg = next->string();
        // Files directly in .git (config, FETCH_HEAD, …) are watched: `config`
        // is the credential-helper vector and is a trusted shape in its own
        // right. Only the subdirectories are filtered.
        if (std::next(next) == p.end()) return false;
        return std::ranges::find(kGitWatch, seg) == std::ranges::end(kGitWatch);
    }
    (void)in_git;
    return false;
}

// Depth cap. A trusted path matters because a HOST tool finds and runs it, and
// the tools that do that (editors, git, direnv, build drivers) look near the
// root. Eight levels covers every real case while keeping a pathological tree
// from turning a shell call into a full walk.
constexpr int kMaxDepth = 8;

// Entries scanned before we stop. A bound, not a tuning knob: the snapshot runs
// on every shell call, so an unbounded walk would make a big repo pay for the
// gate on each one. Hitting it degrades detection on that call rather than
// stalling the turn -- logged, because silently scanning less is the kind of
// thing that makes a wall quietly stop working.
constexpr std::size_t kMaxEntries = 20000;

[[nodiscard]] std::int64_t mtime_ns(const fs::directory_entry& e) {
    std::error_code ec;
    auto t = e.last_write_time(ec);
    if (ec) return 0;
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               t.time_since_epoch())
        .count();
}

}  // namespace

TrustedSnapshot snapshot_trusted(std::string_view root) {
    TrustedSnapshot snap;
    if (root.empty()) return snap;

    // Allow means "do not gate", so there is nothing to report and no reason to
    // pay for the stats. Same early-out as check(): a user who turned this off
    // did not opt into the cost either.
    const auto cfg = sandbox::config_snapshot();
    if (cfg->handoff == sandbox_cfg::HandoffPolicy::Allow) return snap;

    std::error_code ec;
    fs::path base{root};
    if (!fs::is_directory(base, ec)) return snap;

    std::size_t seen = 0;
    bool truncated = false;

    // Remember the directories we walked, so review() can re-scan them and
    // notice a trusted file that did not exist at snapshot time.
    //
    // Without this the gate only catches MODIFICATIONS: `echo x >
    // .git/hooks/post-commit` creating a brand-new hook was invisible, which
    // is the more dangerous half -- a fresh hook is the classic escape, and an
    // empty repo has no hooks to modify. Verified live before fixing.
    snap.roots.push_back(base.string());

    fs::recursive_directory_iterator it{
        base,
        fs::directory_options::skip_permission_denied, ec};
    if (ec) return snap;

    for (; it != fs::recursive_directory_iterator{}; ++it) {
        if (++seen > kMaxEntries) { truncated = true; break; }

        const fs::directory_entry& e = *it;
        std::error_code dec;
        const bool is_dir = e.is_directory(dec);

        if (is_dir && !dec) {
            const std::string name = e.path().filename().string();
            if (std::ranges::find(kSkipDirs, name) != std::ranges::end(kSkipDirs)) {
                it.disable_recursion_pending();
                continue;
            }
            // Prune unwatched git subtrees rather than filtering them after the
            // fact: .git/objects is thousands of files on any real repo, and
            // walking it only to discard it is the cost without the benefit.
            if (git_noise(e.path())) {
                it.disable_recursion_pending();
                continue;
            }
            if (it.depth() >= kMaxDepth) it.disable_recursion_pending();
            // A directory whose own path is trusted-shaped (.git/hooks,
            // .vscode, .claude) is where a NEW trusted file would appear, so
            // record it for the after-scan.
            sandbox_cfg::TrustKind dk{};
            if (sandbox_cfg::is_host_trusted(e.path().string() + "/probe", &dk))
                snap.roots.push_back(e.path().string());
            continue;   // directories are watched through the files inside them
        }

        const std::string p = e.path().string();
        sandbox_cfg::TrustKind kind{};
        if (!sandbox_cfg::is_host_trusted(p, &kind)) continue;
        if (git_noise(e.path())) continue;   // .git/objects and friends

        TrustedSnapshot::Entry entry;
        entry.path = p;
        entry.kind = kind;
        entry.existed = true;
        std::error_code sec;
        entry.size = e.file_size(sec);
        if (sec) entry.size = 0;
        entry.mtime_ns = mtime_ns(e);
        snap.entries.push_back(std::move(entry));
    }

    if (truncated)
        AGT_LOG(General, Warn, "handoff",
                "trusted snapshot truncated at {} entries under {} -- a write "
                "below the cut would not be reported",
                kMaxEntries, std::string{root});

    AGT_LOG(General, Debug, "handoff", "snapshot {} trusted path(s) under {}",
            snap.entries.size(), std::string{root});
    return snap;
}

std::size_t review_trusted(const TrustedSnapshot& before, std::string_view tool) {
    if (before.empty()) return 0;

    std::size_t recorded = 0;
    for (const auto& e : before.entries) {
        std::error_code ec;
        const bool exists = fs::is_regular_file(e.path, ec);

        const char* what = nullptr;
        if (!exists) {
            what = "deleted";
        } else {
            std::error_code sec, tec;
            const auto sz = fs::file_size(e.path, sec);
            const auto t = fs::last_write_time(e.path, tec);
            const auto ns = tec ? std::int64_t{0}
                                : std::chrono::duration_cast<std::chrono::nanoseconds>(
                                      t.time_since_epoch())
                                      .count();
            // Size OR mtime: a same-length rewrite is the case a size check
            // alone misses, and flipping one character in a hook script is
            // exactly that.
            if ((!sec && sz != e.size) || ns != e.mtime_ns) what = "modified";
        }
        if (!what) continue;

        sandbox_cfg::TrustHandoff h;
        h.kind = e.kind;
        h.write.path = e.path;
        // The command, not the tool name: "shell" tells a user nothing, and
        // WriteRecord exists to answer "who asked". This is the one place the
        // opaque string is genuinely useful -- not for deciding, which is why
        // parsing it was rejected, but for EXPLAINING after the fact.
        h.write.command = std::string{tool};
        h.write.at_ms = now_ms();
        h.write.created = !e.existed;
        remember(h);
        ++recorded;

        AGT_LOG(General, Warn, "handoff",
                "{} a host-trusted path outside the gate: {} ({})",
                what, e.path, sandbox_cfg::explain(e.kind));
    }

    // Second pass: a trusted file that did not exist when we snapshotted.
    //
    // `entries` can only report on paths it already saw, so without this the
    // gate catches an overwritten hook and misses a brand-new one -- the
    // easier and more dangerous move. Bounded the same way the snapshot is:
    // only the directories it actually walked, non-recursively, so this costs
    // one readdir per trusted-shaped directory rather than another tree walk.
    std::vector<std::string> known;
    known.reserve(before.entries.size());
    for (const auto& e : before.entries) known.push_back(e.path);
    std::ranges::sort(known);

    for (const auto& dir : before.roots) {
        std::error_code dec;
        fs::directory_iterator dit{
            dir, fs::directory_options::skip_permission_denied, dec};
        if (dec) continue;
        for (; dit != fs::directory_iterator{}; ++dit) {
            std::error_code fec;
            if (!dit->is_regular_file(fec) || fec) continue;
            const std::string p = dit->path().string();
            if (std::ranges::binary_search(known, p)) continue;   // pre-existing
            sandbox_cfg::TrustKind kind{};
            if (!sandbox_cfg::is_host_trusted(p, &kind)) continue;

            sandbox_cfg::TrustHandoff h;
            h.kind = kind;
            h.write.path = p;
            h.write.command = std::string{tool};
            h.write.at_ms = now_ms();
            h.write.created = true;
            remember(h);
            ++recorded;

            AGT_LOG(General, Warn, "handoff",
                    "created a host-trusted path outside the gate: {} ({})",
                    p, sandbox_cfg::explain(kind));
        }
    }
    return recorded;
}

}  // namespace agentty::tools::util::handoff
