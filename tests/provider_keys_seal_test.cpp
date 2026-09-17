// provider_keys_seal_test — the credential-homogenisation guarantee: NO
// provider key (hosted preset, custom host, or the keyless localhost row)
// may rest as plaintext anywhere on disk. settings.json must carry no
// credential-shaped field at all; the sealed vault (provider-keys.json,
// crypt::seal envelope — the same machinery as credentials.json) is the
// only at-rest home, and a legacy plaintext settings body is imported then
// stripped on the next save.
//
// Isolated like credentials_test: temp $AGENTTY_HOME, keystore disabled.
#include "agtest.hpp"

#include "agentty/auth/cred_crypt.hpp"
#include "agentty/auth/keys.hpp"
#include "agentty/io/persistence.hpp"

#include <cstdlib>
#include <unistd.h>
#include <filesystem>
#include <fstream>
#include <string>

using namespace agentty;
namespace fs = std::filesystem;

namespace {

struct TmpHome {
    fs::path    dir;
    std::string old_home;
    bool        had_home = false;
    TmpHome() {
        dir = fs::temp_directory_path()
            / ("agentty_keys_vault_test_" + std::to_string(::getpid()));
        fs::remove_all(dir);
        fs::create_directories(dir / "home");
        const char* h = ::getenv("AGENTTY_HOME");
        had_home = h != nullptr;
        if (had_home) old_home = h;
        ::setenv("AGENTTY_HOME", (dir / "home").c_str(), 1);
        // Hermetic: an enabled OS keystore would hold the envelope outside
        // the sandbox; the tests assert on the FILE, so force it off.
        ::setenv("AGENTTY_USE_KEYSTORE", "0", 1);
    }
    ~TmpHome() {
        if (had_home) ::setenv("AGENTTY_HOME", old_home.c_str(), 1);
        else          ::unsetenv("AGENTTY_HOME");
        fs::remove_all(dir);
    }
    [[nodiscard]] fs::path settings_file() const {
        return dir / "home" / "settings.json";
    }
    [[nodiscard]] fs::path vault_file() const {
        return dir / "home" / "credentials" / "provider-keys.json";
    }
    static std::string slurp(const fs::path& p) {
        std::ifstream ifs(p, std::ios::binary);
        if (!ifs) return {};
        return {std::istreambuf_iterator<char>(ifs),
                std::istreambuf_iterator<char>()};
    }
};

} // namespace

TEST_CASE("provider keys seal: settings.json carries no plaintext key") {
    TmpHome home;

    {
        store::Settings s;
        s.provider_keys["groq"] = "gsk-live-groq-secret-key-0000000000";
        s.provider_keys["https://chat.example.org/api"] = "sk-custom-host-key";
        s.provider_keys["localhost:8080"] = "";   // keyless localhost row
        persistence::save_settings(s);
    }

    // The settings body must be credential-free — not even an empty
    // "provider_keys" object, so a pasted bug report / backup of it
    // carries nothing to attack.
    const std::string body = TmpHome::slurp(home.settings_file());
    REQUIRE(!body.empty());
    CHECK(body.find("provider_keys") == std::string::npos);
    CHECK(body.find("gsk-live-groq-secret-key-0000000000") == std::string::npos);
    CHECK(body.find("sk-custom-host-key") == std::string::npos);

    // The vault exists, is machine-bound sealed, and holds no plaintext.
    const std::string vault = TmpHome::slurp(home.vault_file());
    REQUIRE(!vault.empty());
    CHECK(auth::crypt::looks_sealed(vault));
    CHECK(vault.find("gsk-live-groq-secret-key-0000000000") == std::string::npos);
    CHECK(auth::crypt::unseal(vault).has_value());

    // Round-trip: the map comes back byte-identical, keyless row included.
    auto loaded = persistence::load_settings();
    CHECK(loaded.provider_keys.size() == 3);
    CHECK(loaded.provider_keys.at("groq")
          == "gsk-live-groq-secret-key-0000000000");
    CHECK(loaded.provider_keys.at("https://chat.example.org/api")
          == "sk-custom-host-key");
    CHECK(loaded.provider_keys.at("localhost:8080") == "");
}

TEST_CASE("provider keys seal: legacy plaintext settings are migrated + stripped") {
    TmpHome home;

    // Simulate a pre-vault install: plaintext keys in settings.json,
    // no vault on disk.
    {
        std::ofstream ofs(home.settings_file(), std::ios::binary | std::ios::trunc);
        ofs << R"({"model_id":"claude-opus-4-5","provider":"groq",)"
            << R"("provider_keys":{"groq":"gsk-legacy-plaintext-key"})"
            << "}";
    }
    CHECK(!fs::exists(home.vault_file()));

    // First load imports the legacy keys into the in-memory map AND seals
    // them into the vault RIGHT THERE — the whole point of sealing at load
    // is that no save need ever run for the plaintext to stop being the
    // only copy. (The reducers hold the cached settings for minutes; a
    // process exit before the next save must not leave the key unsealed.)
    auto s = persistence::load_settings();
    CHECK(s.provider_keys.size() == 1);
    CHECK(s.provider_keys.at("groq") == "gsk-legacy-plaintext-key");
    REQUIRE(fs::exists(home.vault_file()));
    CHECK(auth::crypt::unseal(TmpHome::slurp(home.vault_file())).has_value());

    // …and the first save reseals them (idempotent now) and strips the
    // plaintext object from settings.json entirely.
    persistence::save_settings(s);

    const std::string body = TmpHome::slurp(home.settings_file());
    CHECK(body.find("provider_keys") == std::string::npos);
    CHECK(body.find("gsk-legacy-plaintext-key") == std::string::npos);

    const std::string vault = TmpHome::slurp(home.vault_file());
    REQUIRE(!vault.empty());
    CHECK(auth::crypt::looks_sealed(vault));
    CHECK(auth::crypt::unseal(vault).has_value());
    CHECK(vault.find("gsk-legacy-plaintext-key") == std::string::npos);

    auto after = persistence::load_settings();
    CHECK(after.provider_keys.size() == 1);
    CHECK(after.provider_keys.at("groq") == "gsk-legacy-plaintext-key");
}

TEST_CASE("provider keys seal: sign-out clears the vault at rest") {
    TmpHome home;

    {
        store::Settings s;
        s.provider_keys["groq"] = "gsk-live-groq-secret-key-0000000000";
        persistence::save_settings(s);
    }
    REQUIRE(fs::exists(home.vault_file()));

    // An empty map saved is authoritative — a sign-out (or an in-app
    // key_clear) must leave no recoverable key at rest.
    store::Settings s;
    persistence::save_settings(s);

    CHECK(!fs::exists(home.vault_file()));
    auto loaded = persistence::load_settings();
    CHECK(loaded.provider_keys.empty());
}
