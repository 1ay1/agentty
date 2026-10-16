// accounts_registry_test — the named per-provider account store.
//
// Exercises the registry layer (list/upsert/get/remove/set_active/active_label)
// in a sandboxed XDG_CONFIG_HOME so it never touches real credentials. The
// provider-specific snapshot/activate adapters need live credential files and
// are covered indirectly by the reducer; here we lock the pure registry
// semantics that the switcher UI depends on.

#include "agtest_cap.hpp"
#include "agentty/auth/accounts.hpp"

#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>
#include <sys/wait.h>
#include <unistd.h>   // getpid, fork

#include "agtest.hpp"

namespace fs = std::filesystem;
namespace acc = agentty::auth::accounts;

TEST_CASE("accounts registry list/upsert/get/remove/set_active") {
    // Sandbox: a PID-unique config dir so the suite stays hermetic under -j.
    auto root = fs::temp_directory_path() /
                ("agentty_accounts_" + std::to_string(::getpid()));
    fs::remove_all(root);
    fs::create_directories(root);
    ::setenv("AGENTTY_HOME", root.c_str(), 1);
    ::setenv("HOME", root.c_str(), 1);
    // Keystore is opt-in (AGENTTY_USE_KEYSTORE); leaving it unset keeps the
    // pure file/crypt path, which is what we want to exercise here.
    ::unsetenv("AGENTTY_USE_KEYSTORE");

    // Empty registry.
    CHECK(acc::list(::agentty::IoAccess::grant()).empty(), "fresh registry lists nothing");
    CHECK(acc::active_label(::agentty::IoAccess::grant(), "anthropic").empty(), "no active label initially");
    CHECK(!acc::get(::agentty::IoAccess::grant(), "anthropic", "work").has_value(), "get on empty is nullopt");

    // Upsert two Anthropic accounts + one ChatGPT.
    CHECK(acc::upsert(::agentty::IoAccess::grant(), agtest::cap(), "anthropic", "work",     R"({"method":"oauth"})"), "upsert work");
    CHECK(acc::upsert(::agentty::IoAccess::grant(), agtest::cap(), "anthropic", "personal", R"({"method":"api_key"})"), "upsert personal");
    CHECK(acc::upsert(::agentty::IoAccess::grant(), agtest::cap(), "chatgpt",   "team",     R"({"account_id":"acc_123"})"), "upsert chatgpt");

    // The most recent upsert per provider is active.
    CHECK(acc::active_label(::agentty::IoAccess::grant(), "anthropic") == "personal", "newest upsert is active (anthropic)");
    CHECK(acc::active_label(::agentty::IoAccess::grant(), "chatgpt")   == "team",     "chatgpt active label");

    // list_for scopes by provider.
    CHECK(acc::list_for(::agentty::IoAccess::grant(), "anthropic").size() == 2, "two anthropic accounts");
    CHECK(acc::list_for(::agentty::IoAccess::grant(), "chatgpt").size()   == 1, "one chatgpt account");
    CHECK(acc::list(::agentty::IoAccess::grant()).size() == 3, "three accounts total");

    // get returns the stored secret.
    auto w = acc::get(::agentty::IoAccess::grant(), "anthropic", "work");
    CHECK(w.has_value() && w->secret == R"({"method":"oauth"})", "get returns work secret");

    // Persistence: a fresh read (new process would re-read the file) sees them.
    // We simulate by re-reading through list() which re-parses accounts.json.
    CHECK(acc::path().find("accounts.json") != std::string::npos, "registry path is accounts.json");
    CHECK(fs::exists(acc::path()), "accounts.json written to disk");

    // Switch active explicitly.
    CHECK(acc::set_active(::agentty::IoAccess::grant(), agtest::cap(), "anthropic", "work"), "set_active work");
    CHECK(acc::active_label(::agentty::IoAccess::grant(), "anthropic") == "work", "active is now work");
    CHECK(!acc::set_active(::agentty::IoAccess::grant(), agtest::cap(), "anthropic", "ghost"), "set_active on missing fails");

    // Upsert existing updates secret + bumps active, doesn't duplicate.
    CHECK(acc::upsert(::agentty::IoAccess::grant(), agtest::cap(), "anthropic", "work", R"({"method":"oauth","v":2})"), "re-upsert work");
    CHECK(acc::list_for(::agentty::IoAccess::grant(), "anthropic").size() == 2, "no duplicate row after re-upsert");
    auto w2 = acc::get(::agentty::IoAccess::grant(), "anthropic", "work");
    CHECK(w2 && w2->secret.find("\"v\":2") != std::string::npos, "secret updated in place");

    // Remove the active account → newest remaining is promoted.
    CHECK(acc::remove(::agentty::IoAccess::grant(), agtest::cap(), "anthropic", "work"), "remove active work");
    CHECK(acc::get(::agentty::IoAccess::grant(), "anthropic", "work") == std::nullopt, "work is gone");
    CHECK(acc::active_label(::agentty::IoAccess::grant(), "anthropic") == "personal", "personal promoted to active");
    CHECK(acc::list_for(::agentty::IoAccess::grant(), "anthropic").size() == 1, "one anthropic account left");

    // Remove the last one → active label clears.
    CHECK(acc::remove(::agentty::IoAccess::grant(), agtest::cap(), "anthropic", "personal"), "remove personal");
    CHECK(acc::list_for(::agentty::IoAccess::grant(), "anthropic").empty(), "no anthropic accounts left");
    CHECK(acc::active_label(::agentty::IoAccess::grant(), "anthropic").empty(), "active label cleared when empty");

    // ChatGPT slot untouched by anthropic churn.
    CHECK(acc::list_for(::agentty::IoAccess::grant(), "chatgpt").size() == 1, "chatgpt account still present");

    // Reject empty provider/label.
    CHECK(!acc::upsert(::agentty::IoAccess::grant(), agtest::cap(), "", "x", "s"), "upsert rejects empty provider");
    CHECK(!acc::upsert(::agentty::IoAccess::grant(), agtest::cap(), "anthropic", "", "s"), "upsert rejects empty label");

    fs::remove_all(root);
}

// Several agentty windows logging into accounts at once. Before the registry
// took a cross-process section, each read accounts.json, added its slot and
// renamed its copy over the others' (through ONE shared ".tmp" name, so two
// could even write the same temp): the losers' accounts silently vanished.
TEST_CASE("accounts registry: concurrent processes never lose an account") {
    auto root = fs::temp_directory_path() /
                ("agentty_accounts_x_" + std::to_string(::getpid()));
    fs::remove_all(root);
    fs::create_directories(root);
    ::setenv("AGENTTY_HOME", root.c_str(), 1);
    ::setenv("HOME", root.c_str(), 1);
    ::unsetenv("AGENTTY_USE_KEYSTORE");

    constexpr int kWriters = 6;
    constexpr int kEach    = 8;
    std::vector<::pid_t> kids;
    for (int w = 0; w < kWriters; ++w) {
        const ::pid_t pid = ::fork();
        if (pid == 0) {
            bool ok = true;
            for (int i = 0; i < kEach; ++i)
                ok = acc::upsert(::agentty::IoAccess::grant(), agtest::cap(), "anthropic",
                                 "w" + std::to_string(w) + "-" + std::to_string(i),
                                 R"({"method":"api_key"})") && ok;
            ::_exit(ok ? 0 : 1);
        }
        if (pid > 0) kids.push_back(pid);
    }
    int failed = 0;
    for (::pid_t p : kids) {
        int st = 0;
        ::waitpid(p, &st, 0);
        if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) ++failed;
    }
    CHECK(failed == 0, "every writer's upserts reported success");
    const auto n = acc::list_for(::agentty::IoAccess::grant(), "anthropic").size();
    const std::string survived = "all " + std::to_string(kWriters * kEach)
                               + " accounts survived (got " + std::to_string(n) + ")";
    CHECK(n == kWriters * kEach, survived);
    std::size_t temps = 0;
    for (const auto& e : fs::recursive_directory_iterator(root))
        if (e.path().filename().string().find(".tmp") != std::string::npos) ++temps;
    CHECK(temps == 0, "no temp files left behind");

    fs::remove_all(root);
}
