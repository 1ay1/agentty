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
    fs::path dir;
    const char* old_home;
    const char* old_xdg;
    TmpHome() {
        dir = fs::temp_directory_path()
            / ("agentty_cred_test_" + std::to_string(::getpid()));
        fs::remove_all(dir);
        fs::create_directories(dir);
        old_home = ::getenv("HOME");
        old_xdg  = ::getenv("XDG_CONFIG_HOME");
        ::setenv("HOME", dir.c_str(), 1);
        ::setenv("XDG_CONFIG_HOME", dir.c_str(), 1);
    }
    ~TmpHome() {
        if (old_home) ::setenv("HOME", old_home, 1); else ::unsetenv("HOME");
        if (old_xdg)  ::setenv("XDG_CONFIG_HOME", old_xdg, 1);
        else          ::unsetenv("XDG_CONFIG_HOME");
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
