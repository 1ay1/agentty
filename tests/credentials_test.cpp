// credentials_test — the central provider credential layer must resolve the
// RIGHT provider's secret, uniformly. Pins the fix for the class of bug where
// a stale active AuthHeader (Anthropic's OAuth token) was sent to another
// provider (Mistral) → HTTP 401.
//
// Isolated via XDG_CONFIG_HOME + a temp settings dir so it never touches the
// real config; the keystore is disabled so provider_keys round-trip through the
// sealed settings file.
#include "agtest.hpp"

#include "agentty/provider/credentials.hpp"
#include "agentty/auth/auth.hpp"
#include "agentty/io/persistence.hpp"

#include <cstdlib>
#include <unistd.h>
#include <filesystem>
#include <string>

using namespace agentty;
namespace cred = agentty::provider::credentials;
namespace fs = std::filesystem;

namespace {

struct TmpHome {
    fs::path    dir;
    std::string old_home;
    std::string old_agentty;
    bool        had_home    = false;
    bool        had_agentty = false;
    TmpHome() {
        dir = fs::temp_directory_path()
            / ("agentty_cred_test_" + std::to_string(::getpid()));
        fs::remove_all(dir);
        fs::create_directories(dir);
        // COPY the previous values: getenv() returns a pointer INTO the
        // environment block, which setenv() may reallocate — restoring
        // from a stale pointer corrupted the env for every later test in
        // this shared binary (and could leave AGENTTY_HOME unset, which
        // the user-root tripwire then aborts on).
        if (const char* h = ::getenv("HOME"))          { old_home = h; had_home = true; }
        if (const char* a = ::getenv("AGENTTY_HOME"))  { old_agentty = a; had_agentty = true; }
        ::setenv("HOME", dir.c_str(), 1);
        ::setenv("AGENTTY_HOME", dir.c_str(), 1);
    }
    ~TmpHome() {
        if (had_home) ::setenv("HOME", old_home.c_str(), 1);
        else          ::unsetenv("HOME");
        if (had_agentty) ::setenv("AGENTTY_HOME", old_agentty.c_str(), 1);
        else             ::unsetenv("AGENTTY_HOME");
        fs::remove_all(dir);
    }
};

void set_key(const std::string& provider, const std::string& key) {
    auto s = persistence::load_settings();
    s.provider_keys[provider] = key;
    persistence::save_settings(s);
}

} // namespace

TEST_CASE("credentials::resolve returns the target provider's key") {
    TmpHome home;
    // Save DISTINCT keys for two hosted providers.
    set_key("mistral",  "MISTRAL-KEY-32chars-xxxxxxxxDdmj");
    set_key("cerebras", "CEREBRAS-KEY-yyyyyyyyyyyyyyyyyyyy");

    // Resolving mistral must return the mistral key, NOT cerebras' or anything
    // else. (This is exactly the guarantee the 401 bug violated.)
    CHECK(auth::bearer_token(cred::resolve("mistral"))
          == "MISTRAL-KEY-32chars-xxxxxxxxDdmj");
    CHECK(auth::bearer_token(cred::resolve("cerebras"))
          == "CEREBRAS-KEY-yyyyyyyyyyyyyyyyyyyy");
}

TEST_CASE("credentials: local providers need no login, hosted keys do") {
    TmpHome home;
    // A hosted provider with no saved key + no env var needs login.
    CHECK(cred::needs_login("mistral"));
    set_key("mistral", "sk-abc");
    CHECK(!cred::needs_login("mistral"));

    // A local server (ollama) is keyless — never needs login.
    CHECK(!cred::needs_login("ollama"));
    CHECK(cred::add_method("ollama") == cred::AddMethod::None);

    // Hosted key providers take an API key; Anthropic takes OAuth.
    CHECK(cred::add_method("mistral") == cred::AddMethod::ApiKey);
    CHECK(cred::add_method("anthropic") == cred::AddMethod::OAuthDevice);
}

TEST_CASE("credentials::add_key preserves the prior account (no clobber)") {
    TmpHome home;
    set_key("mistral", "key-AAAA");
    // Adding a different key snapshots the old one as an account first.
    cred::add_key("mistral", "key-BBBB");
    CHECK(auth::bearer_token(cred::resolve("mistral")) == "key-BBBB");
    CHECK(cred::list("mistral").size() >= 1);   // the prior key was preserved
}
