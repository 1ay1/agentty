// SPDX-License-Identifier: Apache-2.0
//
// update_check_test.cpp — locks the self-update machinery's pure logic:
// version comparison (the gate for "is an update available"), platform
// asset naming, and the cache-staleness rule (a cache written by an older
// binary must not survive the update it announced).

#include "agentty/util/update.hpp"

#include <doctest/doctest.h>

using agentty::update::version_less;
using agentty::update::platform_asset;
using agentty::update::current_version;

TEST_CASE("version_less: dotted semver ordering") {
    // Strictly newer.
    CHECK(version_less("0.3.0", "0.3.1"));
    CHECK(version_less("0.3.0", "0.4.0"));
    CHECK(version_less("0.3.9", "0.10.0"));   // numeric, not lexicographic
    CHECK(version_less("0.9.9", "1.0.0"));

    // Equal / older — never "available".
    CHECK(!version_less("0.3.0", "0.3.0"));
    CHECK(!version_less("0.3.1", "0.3.0"));
    CHECK(!version_less("1.0.0", "0.9.9"));

    // Leading v tolerated on either side (GitHub tags are vX.Y.Z).
    CHECK(version_less("v0.3.0", "v0.3.1"));
    CHECK(version_less("0.3.0", "v0.3.1"));
    CHECK(!version_less("v0.3.1", "0.3.1"));

    // Missing components are zero.
    CHECK(version_less("0.3", "0.3.1"));
    CHECK(!version_less("0.3.0", "0.3"));

    // Trailing junk after the numeric triple is ignored (pre-release tags
    // compare as their base — conservative: 0.4.0-rc1 counts as 0.4.0).
    CHECK(version_less("0.3.0", "0.4.0-rc1"));

    // Garbage never crashes and compares as 0.0.0.
    CHECK(!version_less("garbage", "0.0.0"));
    CHECK(version_less("garbage", "0.0.1"));
}

TEST_CASE("platform_asset matches release naming") {
    // Whatever platform this test runs on, the asset must be one of the
    // names the release workflow actually publishes.
    auto a = platform_asset();
    CHECK(!a.empty());
    CHECK(a.starts_with("agentty-"));
}

TEST_CASE("current_version is the compiled-in triple") {
    auto v = current_version();
    CHECK(!v.empty());
    // Shape: digits.digits.digits (the project() version).
    int dots = 0;
    for (char c : v) if (c == '.') ++dots;
    CHECK(dots == 2);
}

#include "agentty/auth/auth.hpp"
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>

TEST_CASE("cached_check: fresh cache honoured, stale/foreign rejected") {
    namespace fs = std::filesystem;
    const auto cache = agentty::auth::config_dir() / "update_check.json";
    // Preserve any real cache byte-for-byte.
    std::string saved;
    if (fs::exists(cache)) {
        std::ifstream in(cache, std::ios::binary);
        saved.assign((std::istreambuf_iterator<char>(in)),
                     std::istreambuf_iterator<char>());
    }
    auto now_s = std::chrono::duration_cast<std::chrono::seconds>(
                     std::chrono::system_clock::now().time_since_epoch())
                     .count();
    auto write = [&](const std::string& body) {
        std::ofstream f(cache, std::ios::trunc);
        f << body;
    };

    // 1. Fresh cache written by THIS binary version → honoured.
    write("{\"checked_at\":" + std::to_string(now_s) +
          ",\"current\":\"" + current_version() + "\"," +
          "\"latest\":\"99.0.0\",\"url\":\"u\",\"update_available\":true}");
    auto c = agentty::update::cached_check();
    REQUIRE(c.has_value());
    CHECK(c->update_available);
    CHECK(c->latest == "99.0.0");

    // 2. Cache written by a DIFFERENT (older) binary → rejected: after an
    //    update the new binary must not re-announce the delta it just closed.
    write("{\"checked_at\":" + std::to_string(now_s) +
          ",\"current\":\"0.0.1\"," +
          "\"latest\":\"99.0.0\",\"url\":\"u\",\"update_available\":true}");
    CHECK(!agentty::update::cached_check().has_value());

    // 3. Expired cache (>24h) → rejected.
    write("{\"checked_at\":" + std::to_string(now_s - 90'000) +
          ",\"current\":\"" + current_version() + "\"," +
          "\"latest\":\"99.0.0\",\"url\":\"u\",\"update_available\":true}");
    CHECK(!agentty::update::cached_check().has_value());

    // 4. Corrupt JSON → rejected, no throw.
    write("{not json");
    CHECK(!agentty::update::cached_check().has_value());

    // Restore.
    std::error_code ec;
    if (saved.empty()) fs::remove(cache, ec);
    else { std::ofstream f(cache, std::ios::trunc | std::ios::binary); f << saved; }
}
