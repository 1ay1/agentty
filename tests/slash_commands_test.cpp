// slash_commands_test.cpp — the user-authored slash-command engine
// (tools::commands): discovery across the six roots with shadowing,
// namespaced subdirectory names, lenient frontmatter, and the
// $ARGUMENTS / $1..$9 / $$ substitution contract.
//
// Runs in a temp sandbox CWD + HOME so the real user's commands never
// leak in. Pure filesystem + string logic — no Model, no network.

#include "agtest.hpp"

#include "agentty/tool/commands.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <print>
#include <string>

#ifdef _WIN32
#include <process.h>
#define getpid _getpid
#else
#include <unistd.h>
#endif

namespace fs = std::filesystem;
namespace cmds = agentty::tools::commands;

namespace {


void write_file(const fs::path& p, const std::string& body) {
    fs::create_directories(p.parent_path());
    std::ofstream f(p, std::ios::binary);
    f << body;
}

// Each test mutates the sandbox then invalidates the cache; the mtime
// signature would usually catch it anyway, but same-second writes on a
// coarse-mtime filesystem could alias — the explicit invalidate makes
// the tests deterministic.
void rescan() { cmds::invalidate_cache(); }

void discovery_and_shadowing(const fs::path& sandbox, const fs::path& home) {
    std::println("--- discovery_and_shadowing ---");

    write_file(sandbox / ".agentty" / "commands" / "review.md",
               "---\ndescription: project review\nargument-hint: <file>\n---\n"
               "Review $1 carefully.");
    write_file(sandbox / ".claude" / "commands" / "review.md",
               "CLAUDE ROOT SHADOWED BODY");
    write_file(home / ".agentty" / "commands" / "review.md",
               "USER ROOT SHADOWED BODY");
    write_file(home / ".claude" / "commands" / "deploy.md",
               "Deploy the service: $ARGUMENTS");
    rescan();

    const auto& all = cmds::all();
    check(all.size() == 2, "two distinct names discovered (got "
                           + std::to_string(all.size()) + ")");

    const auto* review = cmds::find("review");
    check(review != nullptr, "review found");
    if (review) {
        check(review->source == "project", "project shadows user+claude roots");
        check(review->body == "Review $1 carefully.",
              ".agentty root wins over .claude and ~/");
        check(review->description == "project review", "frontmatter description");
        check(review->argument_hint == "<file>", "frontmatter argument-hint");
    }
    const auto* deploy = cmds::find("deploy");
    check(deploy != nullptr, "user-root .claude command found (Claude compat)");
    if (deploy) check(deploy->source == "user", "deploy is user-sourced");
    std::println("PASS\n");
}

void namespaced_subdirs(const fs::path& sandbox) {
    std::println("--- namespaced_subdirs ---");
    write_file(sandbox / ".agentty" / "commands" / "git" / "fixup.md",
               "Create a fixup commit for $1.");
    rescan();
    const auto* c = cmds::find("git:fixup");
    check(c != nullptr, "git/fixup.md discovered as git:fixup");
    std::println("PASS\n");
}

void no_frontmatter_fallback(const fs::path& sandbox) {
    std::println("--- no_frontmatter_fallback ---");
    write_file(sandbox / ".agentty" / "commands" / "bare.md",
               "Just a bare prompt line.\nSecond line.");
    rescan();
    const auto* c = cmds::find("bare");
    check(c != nullptr, "frontmatter-less file loads");
    if (c) {
        check(c->body == "Just a bare prompt line.\nSecond line.",
              "whole file is the body");
        check(c->description == "Just a bare prompt line.",
              "description falls back to first body line");
    }
    std::println("PASS\n");
}

void substitution() {
    std::println("--- substitution ---");
    check(cmds::expand("A $1 B $2 C", "x y") == "A x B y C", "$1/$2 positional");
    check(cmds::expand("[$ARGUMENTS]", "  a b  c ") == "[a b  c]",
          "$ARGUMENTS is the trimmed raw string (inner spacing kept)");
    check(cmds::expand("$3", "a b") == "", "absent positional is empty");
    check(cmds::expand("cost: $$5 and $1", "gold") == "cost: $5 and gold",
          "$$ escapes to a literal dollar");
    check(cmds::expand("$unknown stays", "x") == "$unknown stays",
          "unknown $word is literal");
    check(cmds::expand("end $", "x") == "end $", "trailing $ is literal");
    check(cmds::expand("$1$2$1", "a b") == "aba", "adjacent + repeated");
    std::println("PASS\n");
}

void try_expand_shapes(const fs::path& sandbox) {
    std::println("--- try_expand_shapes ---");
    write_file(sandbox / ".agentty" / "commands" / "echo.md", "Echo: $ARGUMENTS");
    rescan();

    auto r = cmds::try_expand("/echo hello world");
    check(r.has_value() && *r == "Echo: hello world", "basic /name args");

    r = cmds::try_expand("/echo");
    check(r.has_value() && *r == "Echo: ",
          "bare /name: empty $ARGUMENTS, template's own spacing is kept "
          "verbatim (got '" + r.value_or("<none>") + "')");

    check(!cmds::try_expand("/etc/hosts is interesting").has_value(),
          "/unknown-name falls through as plain text");
    check(!cmds::try_expand(" /echo hi").has_value(),
          "leading whitespace means prose, not a command");
    check(!cmds::try_expand("/").has_value(), "bare slash is prose");
    check(!cmds::try_expand("no slash").has_value(), "plain text untouched");
    std::println("PASS\n");
}

} // namespace

TEST_CASE("slash commands") {
    agtest::ScopedEnvSandbox _env_guard;
    // Sandbox CWD + HOME.
    const fs::path sandbox =
        fs::temp_directory_path() / ("agentty_cmds_test_" +
                                     std::to_string(::getpid()));
    const fs::path home = sandbox / "home";
    fs::create_directories(home);
    fs::current_path(sandbox);
#ifdef _WIN32
    _putenv_s("USERPROFILE", home.string().c_str());
#else
    ::setenv("HOME", home.string().c_str(), 1);
#endif

    std::println("=== slash_commands_test ===");
    discovery_and_shadowing(sandbox, home);
    namespaced_subdirs(sandbox);
    no_frontmatter_fallback(sandbox);
    substitution();
    try_expand_shapes(sandbox);

    std::error_code ec;
    fs::current_path(fs::temp_directory_path(), ec);
    fs::remove_all(sandbox, ec);
}
