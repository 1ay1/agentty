// shell_env_floor_test — model-supplied env must not be a way to run code
// the user never approved.
//
// ── WHY ──────────────────────────────────────────────────────────────────
//
// `shell` accepts an `env` object and layers it over the child environment,
// caller-wins. Nothing filtered the KEY. So:
//
//     shell(command="git status", env={"GIT_SSH_COMMAND": "/tmp/mine"})
//
// passes every command check -- the command really is `git status` -- and the
// consent card showed only `command`, so the user approved "git status" and
// got /tmp/mine executed. Same shape as CVE-2026-55743 (allowlist bypass via
// an env assignment the checker skipped past).
//
// Two fixes, both needed and neither sufficient alone:
//   - the card now renders cd and env, so consent shows what will run;
//   - the tool refuses the subprocess-execution class of keys outright.
//
// A denylist can't be complete (GOFLAGS=-toolexec=, RUSTC_WRAPPER, the next
// one nobody has written down yet), so it is a floor and the sandbox is the
// boundary. What it buys: the well-known forms stop working quietly.
#include "agtest.hpp"

#include "agentty/tool/registry.hpp"
#include "agentty/tool/tool.hpp"
#include "agentty/runtime/view/thread/turn/permission.hpp"

#include <nlohmann/json.hpp>
#include <string>

using json = nlohmann::json;

namespace {

// Run `shell` with an env block and report whether it was refused.
bool env_refused(const char* key, const char* value = "/tmp/payload") {
    auto r = agentty::tool::DynamicDispatch::execute(
        "shell", json{{"command", "true"},
                      {"env", json{{key, value}}}});
    if (r.has_value()) return false;
    return r.error().kind == agentty::tools::ErrorKind::InvalidArgs
        && r.error().detail.find("refused") != std::string::npos;
}

} // namespace

TEST_CASE("shell env: the loader hooks are refused") {
    CHECK(env_refused("LD_PRELOAD"));
    CHECK(env_refused("LD_AUDIT"));
    CHECK(env_refused("LD_LIBRARY_PATH"));
    CHECK(env_refused("DYLD_INSERT_LIBRARIES"));
}

TEST_CASE("shell env: the shell startup hooks are refused") {
    CHECK(env_refused("BASH_ENV"));
    CHECK(env_refused("ENV"));
    CHECK(env_refused("PROMPT_COMMAND"));
    CHECK(env_refused("IFS", " "));
}

TEST_CASE("shell env: what git runs for you is refused") {
    // The motivating case: the command stays `git status`.
    CHECK(env_refused("GIT_SSH_COMMAND"));
    CHECK(env_refused("GIT_EXTERNAL_DIFF"));
    CHECK(env_refused("GIT_ASKPASS"));
    CHECK(env_refused("GIT_CONFIG_GLOBAL"));
}

TEST_CASE("shell env: interpreter and toolchain hooks are refused") {
    CHECK(env_refused("PYTHONSTARTUP"));
    CHECK(env_refused("NODE_OPTIONS", "--require=/tmp/x.js"));
    CHECK(env_refused("PERL5OPT"));
    CHECK(env_refused("JAVA_TOOL_OPTIONS"));
    CHECK(env_refused("RUSTC_WRAPPER"));
    CHECK(env_refused("GOFLAGS", "-toolexec=/tmp/x"));
}

TEST_CASE("shell env: PATH is refused") {
    // Repoint PATH and every bare command in the approved string is yours.
    CHECK(env_refused("PATH", "/tmp/bin"));
}

TEST_CASE("shell env: the match is case-insensitive") {
    // Case is not a boundary: the kernel's environ is case-sensitive, but a
    // checker that only knows the uppercase spelling is one `ld_preload`
    // away from useless on any host where the loader is lenient -- and the
    // cost of being strict here is zero.
    CHECK(env_refused("ld_preload"));
    CHECK(env_refused("Git_Ssh_Command"));
}

TEST_CASE("shell env: exported-function smuggling is refused by prefix") {
    CHECK(env_refused("BASH_FUNC_ls%%", "() { rm -rf /; }"));
}

TEST_CASE("shell env: ordinary knobs still work") {
    // The floor must not break the legitimate use. These are why `env`
    // exists: make a build quiet, make a test verbose, force colour.
    CHECK_FALSE(env_refused("CI", "1"));
    CHECK_FALSE(env_refused("RUST_LOG", "debug"));
    CHECK_FALSE(env_refused("NO_COLOR", "1"));
    CHECK_FALSE(env_refused("CLICOLOR_FORCE", "1"));
    CHECK_FALSE(env_refused("MY_APP_TOKEN_NAME", "x"));
}

TEST_CASE("shell env: a refused key fails the call, it is not dropped") {
    // Silently dropping it would run a command that does something subtly
    // different from what was asked, which is worse than a clear no.
    auto r = agentty::tool::DynamicDispatch::execute(
        "shell", json{{"command", "true"},
                      {"env", json{{"LD_PRELOAD", "/tmp/x.so"},
                                   {"CI", "1"}}}});
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error().detail.find("LD_PRELOAD") != std::string::npos);
}

// ── and the consent card has to show it ─────────────────────────────────

TEST_CASE("permission card: the shell card shows cd and env") {
    agentty::ToolUse tc;
    tc.id   = agentty::ToolCallId{"c1"};
    tc.name = agentty::ToolName{"shell"};
    tc.args = json{{"command", "ls -la"},
                   {"cd", "/tmp/elsewhere"},
                   {"env", json{{"CI", "1"}}}};
    agentty::PendingPermission pp{tc.id, tc.name, "needs permission", 0};

    const auto cfg = agentty::ui::inline_permission_config(pp, tc);

    CHECK(cfg.description.find("ls -la") != std::string::npos);
    CHECK_MESSAGE(cfg.description.find("/tmp/elsewhere") != std::string::npos,
                  "cd changes what the command acts on, so it is part of the "
                  "decision");
    CHECK_MESSAGE(cfg.description.find("CI=1") != std::string::npos,
                  "env is part of what executes; a card that hides it asks "
                  "for consent to something other than what runs");
}

TEST_CASE("permission card: a plain shell call reads exactly as before") {
    // No cd, no env -> no decoration. The common case stays clean.
    agentty::ToolUse tc;
    tc.id   = agentty::ToolCallId{"c1"};
    tc.name = agentty::ToolName{"shell"};
    tc.args = json{{"command", "cargo test"}};
    agentty::PendingPermission pp{tc.id, tc.name, "needs permission", 0};

    const auto cfg = agentty::ui::inline_permission_config(pp, tc);
    CHECK(cfg.description == "cargo test");
}
