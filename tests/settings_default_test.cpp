// settings_default_test — pins the SHIPPED default permission profile.
//
// A fresh install (no settings.json) and a legacy/partial config missing the
// "profile" key must BOTH resolve to Write — the Profile enum's zero value.
// This is an intentional product decision: agentty is autonomous-by-default,
// contained by the sandbox + workspace boundary (the website documents it at
// /docs/profiles, and the ACP bridge separately starts in Ask). This test is
// the regression lock so a later enum reorder or a Settings-struct-default
// change can't silently flip the default and desync the docs.
//
// It drives the PUBLIC persistence API (load_settings / save_settings)
// against an isolated $HOME so it touches real disk, not a mock.

#include "agentty/io/persistence.hpp"
#include "agentty/store/store.hpp"
#include "agentty/domain/profile.hpp"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

#include <unistd.h>   // getpid

namespace fs = std::filesystem;
using namespace agentty;

// Compile-time twin of the runtime checks below (also enforced project-wide in
// domain/profile.hpp): Write must stay the enum's zero value, because
// load_settings() defaults a missing "profile" key to value("profile", 0).
static_assert(static_cast<std::uint8_t>(Profile::Write) == 0,
              "Profile::Write must remain the enum's zero value — the settings "
              "loader defaults a missing 'profile' key to 0, which must == Write.");

static int g_fails = 0;
static void check(bool ok, const char* what) {
    std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++g_fails;
}

int main() {
    // Isolate the data dir: persistence::data_dir() resolves $HOME/.agentty.
    auto tmp = fs::temp_directory_path()
             / ("agentty_settings_test_" + std::to_string(::getpid()));
    fs::remove_all(tmp);
    fs::create_directories(tmp);
    ::setenv("HOME", tmp.c_str(), 1);
    ::unsetenv("USERPROFILE");   // data_dir() prefers this on Windows; clear it

    const auto settings_json = tmp / ".agentty" / "settings.json";

    // (1) The struct default — the value a default-constructed Settings holds.
    check(store::Settings{}.profile == Profile::Write,
          "Settings{}.profile defaults to Write");

    // (2) Fresh install: nothing persisted yet → load falls back to Write.
    check(!fs::exists(settings_json), "no settings.json present (fresh install)");
    check(persistence::load_settings().profile == Profile::Write,
          "load_settings() with no file -> Write");

    // (3) Legacy/partial config that predates the "profile" key → Write.
    fs::create_directories(tmp / ".agentty");
    {
        std::ofstream ofs(settings_json, std::ios::trunc);
        ofs << R"({"model_id":"claude-x","favorite_models":[]})";
    }
    check(persistence::load_settings().profile == Profile::Write,
          "load_settings() with settings.json missing 'profile' -> Write");

    // (4) A non-default choice still round-trips — proves the Write default
    //     isn't masking a broken parser (Ask/Minimal persist and reload).
    {
        store::Settings s; s.profile = Profile::Ask;
        persistence::save_settings(s);
    }
    check(persistence::load_settings().profile == Profile::Ask,
          "save/load round-trips a non-default profile (Ask)");
    {
        store::Settings s; s.profile = Profile::Minimal;
        persistence::save_settings(s);
    }
    check(persistence::load_settings().profile == Profile::Minimal,
          "save/load round-trips Minimal");

    fs::remove_all(tmp);

    std::printf("%s\n", g_fails == 0 ? "PASSED" : "FAILED");
    return g_fails == 0 ? 0 : 1;
}
