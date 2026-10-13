// keystore_test — OS keystore round-trip.
//
// The keystore is opt-in (AGENTTY_USE_KEYSTORE) and backend-dependent
// (libsecret / macOS security / WinCred). When no backend is available — the
// common case in headless CI — the test SKIPS with success rather than
// failing, so it never blocks a build lacking a Secret Service / keychain.
// To actually exercise the round-trip, run with AGENTTY_USE_KEYSTORE=1 on a
// host with an unlocked keyring.
#include "agentty/auth/keystore.hpp"

#include <cstdio>
#include <string>

namespace ks = agentty::auth::keystore;

static int g_fail = 0;
#define CHECK(cond, msg)                                                       \
    do {                                                                       \
        if (!(cond)) { std::printf("  FAIL: %s\n", msg); ++g_fail; }           \
        else         { std::printf("  ok:   %s\n", msg); }                     \
    } while (0)

int main() {
    std::printf("keystore backend: %s (available=%d)\n",
                ks::backend_name().c_str(), (int)ks::available());

    if (!ks::available()) {
        std::printf("SKIP: no keystore backend enabled/available "
                    "(set AGENTTY_USE_KEYSTORE=1 on a host with a keyring)\n");
        return 0;  // graceful skip
    }

    const std::string key = "agentty-test-item";
    const std::string val = "sealed-{v:2,kdf:argon2id,blob:...}";

    // Clean any leftover from a previous aborted run.
    ks::remove(key);

    CHECK(ks::store(key, val) == ks::Status::Ok, "store returns Ok");

    std::string got;
    CHECK(ks::retrieve(key, got) == ks::Status::Ok, "retrieve returns Ok");
    CHECK(got == val, "retrieved value matches stored");

    CHECK(ks::remove(key) == ks::Status::Ok, "remove returns Ok");

    std::string gone;
    CHECK(ks::retrieve(key, gone) == ks::Status::NotFound,
          "retrieve after remove returns NotFound");

    if (g_fail == 0) { std::printf("\nKEYSTORE ROUND-TRIP PASSED\n"); return 0; }
    std::printf("\n%d KEYSTORE CHECK(S) FAILED\n", g_fail);
    return 1;
}
