// oauth_proactive_refresh_test — the proactive-refresh probe's decision logic.
//
// auth::oauth_proactive_refresh_token() reads the on-disk credential and
// decides whether to hand back a refresh_token so the reducer can pre-refresh
// a soon-to-lapse OAuth token BEFORE the next request would 401 (the
// first-message-lag fix's sibling to the socket prewarm). This exercises the
// real save→probe round-trip through a temp config dir and asserts the window
// boundaries plus the negative cases (no expiry / no refresh_token / API key).
//
// A doctest case in the consolidated binary (links the shared object set, so
// auth.cpp comes for free). It points XDG_CONFIG_HOME at a unique temp dir and
// disables the keystore so save/load go straight to the sealed file.
#include <doctest/doctest.h>

#include "agentty/auth/auth.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <string>

namespace {

namespace auth = agentty::auth;
namespace fs   = std::filesystem;

std::int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

void save_oauth(std::int64_t expires_at_ms, const std::string& refresh) {
    auth::cred::OAuth o;
    o.access_token  = "at-xyz";
    o.refresh_token = refresh;
    o.expires_at_ms = expires_at_ms;
    (void)auth::save_credentials(auth::Credentials{std::move(o)});
}

void isolate_config_dir() {
    static bool done = false;
    if (done) return;
    done = true;
    auto dir = fs::temp_directory_path() /
               ("agentty_proactive_" + std::to_string(now_ms()));
    fs::create_directories(dir);
#if defined(_WIN32)
    _putenv_s("AGENTTY_HOME", dir.string().c_str());
    _putenv_s("AGENTTY_USE_KEYSTORE", "0");
#else
    ::setenv("AGENTTY_HOME", dir.string().c_str(), 1);
    ::setenv("AGENTTY_USE_KEYSTORE", "0", 1);
#endif
}

} // namespace

TEST_CASE("oauth proactive refresh: window + negative cases") {
    isolate_config_dir();
    const std::int64_t t = now_ms();

    SUBCASE("fresh token (30m out) → no proactive refresh") {
        save_oauth(t + 30 * 60 * 1000, "rt-fresh");
        CHECK(!auth::oauth_proactive_refresh_token().has_value());
    }

    SUBCASE("near-expiry token (2m out) → returns the refresh_token") {
        save_oauth(t + 2 * 60 * 1000, "rt-soon");
        auto tok = auth::oauth_proactive_refresh_token();
        REQUIRE(tok.has_value());
        CHECK(*tok == "rt-soon");
    }

    SUBCASE("already-expired token → still returns the refresh_token") {
        save_oauth(t - 60 * 1000, "rt-expired");
        auto tok = auth::oauth_proactive_refresh_token();
        REQUIRE(tok.has_value());
        CHECK(*tok == "rt-expired");
    }

    SUBCASE("near-expiry but empty refresh_token → nothing to refresh with") {
        save_oauth(t + 60 * 1000, "");
        CHECK(!auth::oauth_proactive_refresh_token().has_value());
    }

    SUBCASE("no expiry info (expires_at_ms == 0) → skip") {
        save_oauth(0, "rt-noexp");
        CHECK(!auth::oauth_proactive_refresh_token().has_value());
    }

    SUBCASE("custom window boundaries") {
        save_oauth(t + 4 * 60 * 1000, "rt-window");
        // 4 min out is fresh under a 1-min window …
        CHECK(!auth::oauth_proactive_refresh_token(60 * 1000).has_value());
        // … but stale under a 10-min window.
        auto tok = auth::oauth_proactive_refresh_token(10 * 60 * 1000);
        REQUIRE(tok.has_value());
        CHECK(*tok == "rt-window");
    }

    SUBCASE("API-key credential is not refreshable") {
        (void)auth::save_credentials(
            auth::Credentials{auth::cred::ApiKey{"sk-key"}});
        CHECK(!auth::oauth_proactive_refresh_token().has_value());
    }
}
