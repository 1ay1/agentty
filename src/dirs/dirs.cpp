// agentty::dirs — the write-side storage primitive. See dirs.hpp for the
// contract and for why it is shaped like scope's.
//
// This file is the impure edge: it reads the environment, stats the disk and
// creates directories. The Spec/Lifecycle types stay pure and testable.

#include "agentty/dirs/dirs.hpp"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <set>
#include <utility>

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
void warn_once(std::string_view env, const fs::path& bad,
               const fs::path& fallback, const std::error_code& ec) {
    static std::mutex mu;
    static std::set<std::string, std::less<>> warned;
    {
        std::scoped_lock lk{mu};
        if (!warned.emplace(env).second) return;
    }

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

    const fs::path dflt = *root / fs::path{spec.leaf};

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

}  // namespace agentty::dirs
