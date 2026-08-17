// scope_test — the config-resolution algebra (include/agentty/scope/scope.hpp).
//
// Exercises the pure surface with fabricated Env values (the whole point of
// the purity contract: no filesystem, no getenv). Covers precedence ordering
// in plan(), the two fold monoids (resolve_first override, resolve_union
// shadow+provenance), and content-bound trust (the MCPoison fix).

#include "agentty/scope/scope.hpp"

#include <cassert>
#include <string>
#include <vector>

using namespace agentty::scope;

namespace {

// An Env with both a project root and a home — so plan() emits the full
// Project×Dialect ▷ User×Dialect ladder.
Env full_env() {
    Env e;
    e.home             = "/home/u";
    e.project_root     = "/work/repo";
    e.project_writable = true;
    return e;
}

const Layout kMem{.leaf = "memory.jsonl"};

}  // namespace

// ── plan(): precedence order + provenance ─────────────────────────────────
static void plan_orders_by_precedence() {
    auto srcs = plan(kMem, full_env());

    // Project (3 dialects) then User (3 dialects) = 6, Local not emitted.
    assert(srcs.size() == 6 && "project×3 ▷ user×3, no local");

    // Locus is the major key: every Project source precedes every User source.
    bool seen_user = false;
    for (const auto& s : srcs) {
        if (s.locus == Locus::User) seen_user = true;
        assert(!(seen_user && s.locus == Locus::Project)
               && "project must never come after user");
    }

    // Dialect is the minor key: within Project, native (.agentty) is first.
    assert(srcs[0].locus == Locus::Project && srcs[0].dialect == Dialect::Agentty);
    assert(srcs[0].base == std::filesystem::path{"/work/repo/.agentty"});
    assert(srcs[1].dialect == Dialect::Agents);
    assert(srcs[2].dialect == Dialect::Claude);
    assert(srcs[3].locus == Locus::User && srcs[3].dialect == Dialect::Agentty);
    assert(srcs[3].base == std::filesystem::path{"/home/u/.agentty"});

    // Only the native project dir is writable (memory's guard, generalised).
    assert(srcs[0].writable && "project .agentty writable");
    assert(!srcs[1].writable && "interop project dirs never a write target");
}

// A root of "/" is the unrestricted-access sentinel, never a place to scatter
// .agentty state → no Project sources at all.
static void plan_rejects_filesystem_root() {
    Env e = full_env();
    e.project_root = "/";
    auto srcs = plan(kMem, e);
    for (const auto& s : srcs)
        assert(s.locus != Locus::Project && "\"/\" yields no project locus");
    assert(srcs.size() == 3 && "only user×3 remain");
}

// Explicit ($ENV-pointed) config outranks everything, dialect-agnostic.
static void plan_explicit_wins() {
    Env e = full_env();
    e.explicit_config = std::filesystem::path{"/etc/agentty/mcp.json"};
    auto srcs = plan(kMem, e);
    assert(srcs.front().locus == Locus::Explicit);
    assert(srcs.front().base == std::filesystem::path{"/etc/agentty"});
}

// No HOME (cron/systemd) → no User sources; folds must not crash.
static void plan_no_home() {
    Env e = full_env();
    e.home.clear();
    auto srcs = plan(kMem, e);
    for (const auto& s : srcs)
        assert(s.locus != Locus::User);
    assert(srcs.size() == 3 && "only project×3 remain");
}

// ── resolve_first: override monoid ────────────────────────────────────────
static void resolve_first_picks_first_present() {
    // Pretend only the User/native source "has a value". The override fold
    // must still return the FIRST present one in precedence order — here the
    // project sources are absent (nullopt), so user wins, tagged correctly.
    auto loader = [](const Source& s) -> Result<std::optional<std::string>> {
        if (s.locus == Locus::User && s.dialect == Dialect::Agentty)
            return std::optional<std::string>{"from-user"};
        return std::optional<std::string>{std::nullopt};  // absent
    };
    auto r = resolve_first<std::string>(kMem, full_env(), loader);
    assert(r.has_value());
    assert(r->value == "from-user");
    assert(r->source.locus == Locus::User && "provenance preserved");
}

static void resolve_first_prefers_higher_locus() {
    // Both project and user "present" → project (earlier) wins.
    auto loader = [](const Source& s) -> Result<std::optional<std::string>> {
        if (s.dialect != Dialect::Agentty)
            return std::optional<std::string>{std::nullopt};
        return std::optional<std::string>{
            std::string{to_string(s.locus)} + "-hit"};
    };
    auto r = resolve_first<std::string>(kMem, full_env(), loader);
    assert(r.has_value());
    assert(r->value == "project-hit" && "override picks the strongest locus");
}

static void resolve_first_none_is_error() {
    auto loader = [](const Source&) -> Result<std::optional<std::string>> {
        return std::optional<std::string>{std::nullopt};
    };
    auto r = resolve_first<std::string>(kMem, full_env(), loader);
    assert(!r.has_value());
    assert(r.error().kind == ErrorKind::NoSource);
}

static void resolve_first_propagates_parse_error() {
    // A present-but-broken source short-circuits (fail loud).
    auto loader = [](const Source& s) -> Result<std::optional<std::string>> {
        if (s.locus == Locus::Project && s.dialect == Dialect::Agentty)
            return fail(ErrorKind::ParseFailed, "bad json");
        return std::optional<std::string>{std::nullopt};
    };
    auto r = resolve_first<std::string>(kMem, full_env(), loader);
    assert(!r.has_value());
    assert(r.error().kind == ErrorKind::ParseFailed);
}

// ── resolve_union: shadow + provenance ────────────────────────────────────
static void resolve_union_shadows_by_key() {
    // Every native source yields two items: a common "shared" key and a
    // locus-unique key. First-key-wins → "shared" resolves to the strongest
    // locus (project), and each unique key survives once, tagged with origin.
    auto keyfn  = [](const std::string& v) { return v.substr(0, v.find(':')); };
    auto loader = [](const Source& s) -> Result<std::vector<std::string>> {
        if (s.dialect != Dialect::Agentty) return std::vector<std::string>{};
        std::string tag{to_string(s.locus)};
        return std::vector<std::string>{"shared:" + tag, tag + ":only"};
    };
    auto r = resolve_union<std::string>(kMem, full_env(), keyfn, loader);
    assert(r.has_value());

    // "shared" appears once, from the winning (project) locus.
    int shared = 0;
    for (const auto& t : *r)
        if (keyfn(t.value) == "shared") {
            ++shared;
            assert(t.value == "shared:project" && "project shadows user");
            assert(t.source.locus == Locus::Project);
        }
    assert(shared == 1 && "shadowed to a single winner");

    // Both unique keys survive: shared(1) + project:only + user:only = 3.
    assert(r->size() == 3);
}

// ── trust_of: content-bound, not locus-inferred (MCPoison) ────────────────
static void trust_user_is_implicit() {
    Source user{.locus = Locus::User};
    assert(std::holds_alternative<Trusted>(trust_of(user, "deadbeef", {})));
}

static void trust_project_pending_until_approved() {
    Source proj{.locus = Locus::Project};
    Approvals appr;

    // Unapproved project content → Pending, carrying the hash to approve.
    auto t0 = trust_of(proj, "hash-A", appr);
    assert(std::holds_alternative<Pending>(t0));
    assert(std::get<Pending>(t0).content_sha == "hash-A");

    // Approve THAT hash → Trusted.
    appr.approve("hash-A");
    assert(std::holds_alternative<Trusted>(trust_of(proj, "hash-A", appr)));

    // MCPoison: swap the content under the approved name → approval is VOID.
    auto t2 = trust_of(proj, "hash-B", appr);
    assert(std::holds_alternative<Pending>(t2)
           && "changed content must re-gate trust");
}

int main() {
    plan_orders_by_precedence();
    plan_rejects_filesystem_root();
    plan_explicit_wins();
    plan_no_home();

    resolve_first_picks_first_present();
    resolve_first_prefers_higher_locus();
    resolve_first_none_is_error();
    resolve_first_propagates_parse_error();

    resolve_union_shadows_by_key();

    trust_user_is_implicit();
    trust_project_pending_until_approved();
    return 0;
}
