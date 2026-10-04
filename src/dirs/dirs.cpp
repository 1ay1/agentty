// agentty::dirs — the write-side storage primitive. See dirs.hpp for the
// contract and for why it is shaped like scope's.
//
// This file is the impure edge: it reads the environment, stats the disk and
// creates directories. The Spec/Lifecycle types stay pure and testable.

#include "agentty/dirs/dirs.hpp"

#include <jaal/kernel/guarded.hpp>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <set>
#include <utility>
#include <vector>

#ifndef _WIN32
#include <sys/stat.h>
#include <unistd.h>
#endif

#include "agentty/tool/util/fs_helpers.hpp"
#include "agentty/util/logx.hpp"
#include "agentty/util/user_root.hpp"

namespace agentty::dirs {

namespace {

// Markers that identify a project tree. `.git` first because it is the
// overwhelmingly common case and the walk stops at the first hit, so order
// is a cheap latency win rather than a statement about significance.
//
// `.agentty` is included on purpose: a tree that already has agentty state
// IS the anchor, which makes the anchor stable across the migration in
// step 2 (an existing <cwd>/.agentty keeps being found) rather than
// stranding the very files the sweep would then collect.
constexpr std::array<std::string_view, 4> kMarkers{
    ".git", ".agentty", ".hg", ".svn",
};

// "/" (and "C:\") is agentty's unrestricted-access-boundary sentinel, never
// a place to scatter state. Matches scope::usable_project_root() exactly;
// the two must agree or a path readable by scope becomes unwritable here.
[[nodiscard]] bool usable_project_root(const fs::path& p) noexcept {
    if (p.empty()) return false;
    return p != p.root_path();
}

[[nodiscard]] bool dir_is_writable(const fs::path& p) noexcept {
    std::error_code ec;
    if (!fs::is_directory(p, ec)) return false;
#if defined(_WIN32)
    return true;  // no cheap W_OK; the write itself surfaces failure
#else
    return ::access(p.c_str(), W_OK) == 0;
#endif
}

// Warn at most once per variable, process-wide.
//
// Repeating it per call would be worse than silence: these resolvers are
// called from render and retrieval paths, so a broken override would print
// on every frame and bury the one line that mattered.
//
// jaal::guarded, not a raw mutex + set: access is only possible through
// with(), so "forgot the lock" is unrepresentable rather than a review item.
// Same reasoning as the handoff gate's feed(), and the concurrency banlist
// enforces it.
void warn_once(std::string_view env, const fs::path& bad,
               const fs::path& fallback, const std::error_code& ec) {
    static jaal::guarded<std::set<std::string, std::less<>>> warned;
    // Capture-less AND owned-value arguments, both jaal rules: a capture or a
    // pointer/view could reach a second lock while this one is held, and
    // holding two is how deadlocks start. So the key is copied in.
    if (!warned.with([](auto& seen, std::string k) {
            return seen.emplace(std::move(k)).second;
        }, std::string{env}))
        return;

    // create_directories() leaves `ec` CLEAR when the path already exists
    // as a non-directory -- it did not fail, it had nothing to do -- so
    // reporting ec here printed "Undefined error: 0" for the single most
    // likely mistake (pointing the override at a file). Name that case
    // directly and keep the errno text for everything else. Inherited from
    // user_root's resolve_subdir, which learned it the hard way.
    std::error_code exists_ec;
    const bool in_the_way = fs::exists(bad, exists_ec);
    // Hold the message in a named string: ec.message() returns BY VALUE, so
    // calling .c_str() on the temporary inside a ternary leaves a dangling
    // pointer the moment the full-expression ends.
    const std::string ec_text = ec ? ec.message() : std::string{"cannot create"};
    const std::string why = in_the_way ? std::string{"exists but is not a directory"}
                                       : ec_text;

    std::fprintf(stderr,
                 "agentty: warning: $%.*s='%s' is not usable (%s) — falling back to %s\n",
                 static_cast<int>(env.size()), env.data(),
                 bad.string().c_str(), why.c_str(), fallback.string().c_str());

    AGT_LOG(Persist, Warn, "dirs",
            "override ${} unusable ({}): {} -> falling back to {}",
            env, why, bad.string(), fallback.string());
}

// Resolve the root a Spec hangs off, without touching `leaf`.
[[nodiscard]] std::expected<fs::path, Error> root_for(Root r) {
    switch (r) {
        case Root::User: {
            fs::path p = ::agentty::util::user_root();
            if (p.empty())
                return std::unexpected(Error{Error::Kind::NoRoot,
                                             "no user root (HOME unresolvable)"});
            return p;
        }
        case Root::Project: {
            fs::path a = project_anchor();
            if (a.empty())
                return std::unexpected(Error{Error::Kind::NoRoot,
                                             "no usable project anchor"});
            if (!dir_is_writable(a))
                return std::unexpected(Error{Error::Kind::NotWritable,
                                             "project anchor is not writable: "
                                                 + a.string()});
            return a / ".agentty";
        }
    }
    return std::unexpected(Error{Error::Kind::NoRoot, "unknown root"});
}

// The shared core. `create` distinguishes resolve() from resolve_dry().
[[nodiscard]] std::expected<Resolved, Error> resolve_impl(const Spec& spec,
                                                          bool create) {
    auto root = root_for(spec.root);
    if (!root) return std::unexpected(root.error());

    // An EMPTY leaf means "the root itself" — a caller that owns its own
    // filename (rag appends rag_docs.<tag>.ragdb) and wants the directory.
    //
    // Must not go through operator/: joining an empty path appends a
    // trailing separator, so `root / ""` compares UNEQUAL to `root` even
    // though it names the same directory. That inequality is invisible
    // until something compares two resolved paths, which is exactly what a
    // "did this move?" check does.
    const fs::path dflt = spec.leaf.empty() ? *root
                                            : *root / fs::path{spec.leaf};

    // $env wins when set and NON-EMPTY. An exported-but-blank variable is a
    // common shell accident and must relocate nothing.
    fs::path chosen = dflt;
    bool overridden = false;
    if (!spec.env.empty()) {
        // getenv needs a NUL-terminated string; Spec carries a view.
        const std::string env_name{spec.env};
        if (const char* v = std::getenv(env_name.c_str()); v && *v) {
            fs::path given{v};
            // A RELATIVE override resolves against the ROOT, never the
            // process CWD: AGENTTY_LOGS_DIR=logs2 has to mean one
            // directory, not a different one per launch directory.
            chosen = given.is_absolute() ? std::move(given) : *root / given;
            overridden = true;
        }
    }

    Resolved out;
    out.root = *root;
    out.path = chosen;
    out.origin = overridden ? Origin::Override : Origin::Default;

    if (!create) return out;

    std::error_code ec;
    fs::create_directories(chosen, ec);

    if (!fs::is_directory(chosen, ec)) {
        if (!overridden) {
            // The DEFAULT is unusable. Nothing to fall back to, so this is
            // a hard error rather than a warning -- returning a path we
            // know does not work is how callers end up reporting
            // "Undefined error: 0" from somewhere else entirely.
            return std::unexpected(Error{Error::Kind::Unusable,
                                         "cannot create " + chosen.string()});
        }
        warn_once(spec.env, chosen, dflt, ec);
        chosen = dflt;
        std::error_code dec;
        fs::create_directories(chosen, dec);
        if (!fs::is_directory(chosen, dec))
            return std::unexpected(Error{Error::Kind::Unusable,
                                         "neither override nor default usable: "
                                             + dflt.string()});
        out.path = chosen;
        out.origin = Origin::OverrideFellBack;
    }

#ifndef _WIN32
    // The default inherits 0700 from the root; an override can point
    // anywhere, including a 0755 directory the user already created. This
    // is the one place the bit has to be set explicitly rather than assumed.
    if (spec.owner_only) ::chmod(chosen.c_str(), S_IRWXU);
#endif

    // Make the PROJECT root self-ignoring, once, when we create it.
    //
    // <project>/.agentty holds derived state -- a 33 MB retrieval index in
    // this very repo -- next to a user's source tree. agentty's own
    // .gitignore lists it, but that protects nobody else: in any OTHER
    // checkout the first `git add -A` after a warm index stages tens of
    // megabytes of binary, and the author finds out at review time.
    //
    // Shipping the rule INSIDE the directory makes it structural rather
    // than something every user must know to write. The file ignores its own
    // directory, so it is also the only thing in there git can see.
    //
    // Best-effort and never overwritten: a user who deliberately commits
    // part of .agentty (a shared skills/ dir is a real use) edits this file,
    // and a tool that rewrites it every launch would be a tool that argues.
    if (spec.root == Root::Project && !out.root.empty()) {
        std::error_code gec;
        const fs::path ignore = out.root / ".gitignore";
        if (!fs::exists(ignore, gec)) {
            std::ofstream f(ignore);
            if (f) f << "# agentty's project state. Derived data (retrieval"
                        " indexes, caches)\n"
                        "# lives here and should not be committed.\n"
                        "# Delete a line to start tracking something; agentty"
                        " won't rewrite this.\n"
                        "*\n";
        }
    }

    AGT_LOG(Persist, Debug, "dirs", "resolved {} -> {} ({})",
            spec.leaf, out.path.string(),
            out.origin == Origin::Default         ? "default"
            : out.origin == Origin::Override      ? "override"
                                                  : "override-fell-back");
    return out;
}

}  // namespace

std::string_view to_string(Error::Kind k) noexcept {
    switch (k) {
        case Error::Kind::NoRoot:      return "no-root";
        case Error::Kind::NotWritable: return "not-writable";
        case Error::Kind::Unusable:    return "unusable";
    }
    return "unknown";
}

std::span<const std::string_view> project_markers() noexcept {
    return {kMarkers.data(), kMarkers.size()};
}

fs::path project_anchor() {
    // Start from project_root() -- the cwd, already clamped inside the
    // access boundary -- so the walk can never begin outside the sandbox.
    fs::path start = ::agentty::tools::util::project_root();
    if (start.empty()) return {};

    std::error_code ec;
    fs::path cur = fs::weakly_canonical(start, ec);
    if (ec) cur = start;

    // Walk up to the nearest marker. Stops at the root path, so a tree with
    // no marker at all falls through to the clamped cwd rather than
    // escaping to "/".
    for (fs::path p = cur; usable_project_root(p); p = p.parent_path()) {
        for (std::string_view m : kMarkers) {
            std::error_code mec;
            if (fs::exists(p / fs::path{m}, mec)) {
                AGT_LOG(Persist, Debug, "dirs", "anchor {} (marker {})",
                        p.string(), m);
                return p;
            }
        }
        if (p == p.parent_path()) break;  // defensive: no infinite loop
    }

    // No marker anywhere above us. Fall back to the clamped cwd, which is
    // what every consumer did before this existed -- so a markerless tree
    // behaves exactly as it does today instead of refusing to store.
    if (!usable_project_root(cur)) return {};
    AGT_LOG(Persist, Debug, "dirs", "anchor {} (no marker, cwd fallback)",
            cur.string());
    return cur;
}

std::expected<Resolved, Error> resolve(const Spec& spec) {
    return resolve_impl(spec, /*create=*/true);
}

std::expected<Resolved, Error> resolve_dry(const Spec& spec) {
    return resolve_impl(spec, /*create=*/false);
}

SweepStats sweep(const SweepRequest& req, bool dry_run) {
    SweepStats st;

    // The gate. A store that is not rebuildable is never swept, whatever
    // keep_last says -- see Lifecycle::rebuildable for why that asymmetry
    // is a field rather than a convention.
    if (!req.spec.life.sweeps()) return st;
    if (req.stem.empty() || req.suffix.empty()) return st;

    // resolve_dry: a sweep must not CREATE the directory it is cleaning.
    auto d = resolve_dry(req.spec);
    if (!d) return st;

    std::error_code ec;
    if (!fs::is_directory(d->path, ec)) return st;

    const auto now = fs::file_time_type::clock::now();

    struct Candidate {
        fs::path path;
        fs::file_time_type mtime;
        std::uintmax_t size = 0;
    };
    std::vector<Candidate> cands;
    // Legacy untagged files are tracked SEPARATELY, not ranked with the
    // tagged variants. Putting them in the same list let keep_last protect
    // one -- and since a legacy file is often the newest thing in the
    // directory, it won the slot and a live-adjacent variant got deleted
    // instead. They are unreadable by any current code path, so there is
    // nothing to rank: they always go.
    std::vector<Candidate> legacy;

    // The untagged legacy name: "rag_code." + ".ragdb" -> "rag_code.ragdb".
    std::string untagged;
    if (req.include_untagged) {
        std::string s{req.stem};
        if (!s.empty() && s.back() == '.') s.pop_back();
        untagged = s + std::string{req.suffix};
    }

    fs::directory_iterator it{d->path, fs::directory_options::skip_permission_denied, ec};
    if (ec) {
        // Could not even open the directory. Report and delete nothing.
        st.failed = 1;
        return st;
    }

    for (const auto& entry : it) {
        std::error_code eec;
        if (!entry.is_regular_file(eec) || eec) {
            if (eec) ++st.failed;
            continue;
        }
        const std::string name = entry.path().filename().string();

        bool is_variant = name.size() > req.stem.size() + req.suffix.size()
                       && name.starts_with(req.stem) && name.ends_with(req.suffix);
        const bool is_legacy = !untagged.empty() && name == untagged;
        if (!is_variant && !is_legacy) continue;

        ++st.examined;

        // Live variants survive unconditionally.
        bool live = false;
        for (const auto& k : req.keep)
            if (name == k) { live = true; break; }
        if (live) { ++st.kept; continue; }

        std::error_code tec;
        auto mt = fs::last_write_time(entry.path(), tec);
        std::error_code sec;
        auto sz = fs::file_size(entry.path(), sec);
        if (tec || sec) { ++st.failed; continue; }

        // The grace window. Another process may be mid-save, and a
        // concurrent one may legitimately be running a different config.
        // The legacy untagged file is exempt: no current code path can even
        // name it, so nothing is about to be writing it.
        if (!is_legacy && req.spec.life.min_age.count() > 0
            && now - mt < req.spec.life.min_age) {
            ++st.too_new;
            ++st.kept;
            continue;
        }

        cands.push_back({entry.path(), mt, sz});
        if (is_legacy) {
            legacy.push_back(cands.back());
            cands.pop_back();
        }
    }

    // Anything unreadable means our view of the directory is incomplete, so
    // we cannot say which variants are newest. Delete nothing. Same rule as
    // blobs::gc: being wrong about what is present costs disk, never data.
    if (st.failed > 0) {
        st.kept += cands.size() + legacy.size();
        AGT_LOG(Persist, Warn, "dirs",
                "sweep of {} aborted: {} unreadable entr{}",
                d->path.string(), st.failed, st.failed == 1 ? "y" : "ies");
        return st;
    }

    // Newest first, then keep the N most recent. keep_last counts VARIANTS
    // retained beyond the live ones, which is what makes an A/B between two
    // embedders free: switch back and the previous index is still warm.
    std::sort(cands.begin(), cands.end(),
              [](const Candidate& a, const Candidate& b) { return a.mtime > b.mtime; });

    const std::size_t keep_n = std::min<std::size_t>(req.spec.life.keep_last,
                                                     cands.size());
    st.kept += keep_n;

    // Doomed = the tagged variants beyond keep_last, PLUS every legacy file.
    std::vector<Candidate> doomed;
    doomed.reserve(cands.size() - keep_n + legacy.size());
    for (std::size_t i = keep_n; i < cands.size(); ++i) doomed.push_back(cands[i]);
    for (const auto& l : legacy) doomed.push_back(l);

    for (const auto& c : doomed) {
        // Sidecars go with the principal: a .meta.json describing a file
        // that is gone is a trap every loader then has to defend against.
        std::uintmax_t freed = c.size;
        std::vector<fs::path> victims{c.path};
        for (std::string_view sfx : req.sidecar_suffixes) {
            fs::path side{c.path.string() + std::string{sfx}};
            std::error_code xec;
            if (fs::is_regular_file(side, xec)) {
                std::error_code zec;
                freed += fs::file_size(side, zec);
                victims.push_back(std::move(side));
            }
        }

        if (dry_run) {
            ++st.deleted;
            st.bytes_freed += freed;
            AGT_LOG(Persist, Debug, "dirs", "sweep (dry) would remove {}",
                    c.path.filename().string());
            continue;
        }

        bool all_gone = true;
        for (const auto& v : victims) {
            std::error_code rec;
            fs::remove(v, rec);
            if (rec) all_gone = false;
        }
        if (all_gone) {
            ++st.deleted;
            st.bytes_freed += freed;
            AGT_LOG(Persist, Info, "dirs", "sweep removed {} ({} bytes)",
                    c.path.filename().string(), freed);
        } else {
            ++st.failed;
        }
    }

    if (st.deleted > 0)
        AGT_LOG(Persist, Info, "dirs",
                "sweep {}: examined {} kept {} deleted {} ({} bytes)",
                dry_run ? "dry-run" : "done",
                st.examined, st.kept, st.deleted, st.bytes_freed);
    return st;
}

}  // namespace agentty::dirs
