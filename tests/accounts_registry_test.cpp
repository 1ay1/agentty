// accounts_registry_test — the named per-provider account store.
//
// Exercises the registry layer (list/upsert/get/remove/set_active/active_label)
// in a sandboxed XDG_CONFIG_HOME so it never touches real credentials. The
// provider-specific snapshot/activate adapters need live credential files and
// are covered indirectly by the reducer; here we lock the pure registry
// semantics that the switcher UI depends on.

#include "agentty/auth/accounts.hpp"

#include <cstdlib>
#include <filesystem>
#include <string>
#include <unistd.h>   // getpid

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
    CHECK(acc::list().empty(), "fresh registry lists nothing");
    CHECK(acc::active_label("anthropic").empty(), "no active label initially");
    CHECK(!acc::get("anthropic", "work").has_value(), "get on empty is nullopt");

    // Upsert two Anthropic accounts + one ChatGPT.
    CHECK(acc::upsert("anthropic", "work",     R"({"method":"oauth"})"), "upsert work");
    CHECK(acc::upsert("anthropic", "personal", R"({"method":"api_key"})"), "upsert personal");
    CHECK(acc::upsert("chatgpt",   "team",     R"({"account_id":"acc_123"})"), "upsert chatgpt");

    // The most recent upsert per provider is active.
    CHECK(acc::active_label("anthropic") == "personal", "newest upsert is active (anthropic)");
    CHECK(acc::active_label("chatgpt")   == "team",     "chatgpt active label");

    // list_for scopes by provider.
    CHECK(acc::list_for("anthropic").size() == 2, "two anthropic accounts");
    CHECK(acc::list_for("chatgpt").size()   == 1, "one chatgpt account");
    CHECK(acc::list().size() == 3, "three accounts total");

    // get returns the stored secret.
    auto w = acc::get("anthropic", "work");
    CHECK(w.has_value() && w->secret == R"({"method":"oauth"})", "get returns work secret");

    // Persistence: a fresh read (new process would re-read the file) sees them.
    // We simulate by re-reading through list() which re-parses accounts.json.
    CHECK(acc::path().find("accounts.json") != std::string::npos, "registry path is accounts.json");
    CHECK(fs::exists(acc::path()), "accounts.json written to disk");

    // Switch active explicitly.
    CHECK(acc::set_active("anthropic", "work"), "set_active work");
    CHECK(acc::active_label("anthropic") == "work", "active is now work");
    CHECK(!acc::set_active("anthropic", "ghost"), "set_active on missing fails");

    // Upsert existing updates secret + bumps active, doesn't duplicate.
    CHECK(acc::upsert("anthropic", "work", R"({"method":"oauth","v":2})"), "re-upsert work");
    CHECK(acc::list_for("anthropic").size() == 2, "no duplicate row after re-upsert");
    auto w2 = acc::get("anthropic", "work");
    CHECK(w2 && w2->secret.find("\"v\":2") != std::string::npos, "secret updated in place");

    // Remove the active account → newest remaining is promoted.
    CHECK(acc::remove("anthropic", "work"), "remove active work");
    CHECK(acc::get("anthropic", "work") == std::nullopt, "work is gone");
    CHECK(acc::active_label("anthropic") == "personal", "personal promoted to active");
    CHECK(acc::list_for("anthropic").size() == 1, "one anthropic account left");

    // Remove the last one → active label clears.
    CHECK(acc::remove("anthropic", "personal"), "remove personal");
    CHECK(acc::list_for("anthropic").empty(), "no anthropic accounts left");
    CHECK(acc::active_label("anthropic").empty(), "active label cleared when empty");

    // ChatGPT slot untouched by anthropic churn.
    CHECK(acc::list_for("chatgpt").size() == 1, "chatgpt account still present");

    // Reject empty provider/label.
    CHECK(!acc::upsert("", "x", "s"), "upsert rejects empty provider");
    CHECK(!acc::upsert("anthropic", "", "s"), "upsert rejects empty label");

    fs::remove_all(root);
}
