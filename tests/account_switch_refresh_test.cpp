// account_switch_refresh_test — switching to a long-idle account must not
// leave you holding an EXPIRED token.
//
// Two accounts on the same provider (Anthropic): if you switch to one that's
// been idle long enough for its OAuth access token to lapse, the account_select
// reducer must kick a background token refresh (oauth_refresh_in_flight) using
// the just-swapped store's refresh_token — instead of installing the stale
// bearer verbatim and 401-ing on the first turn. Switching to an account whose
// token is still fresh must NOT trigger a refresh.
//
// Driven through the REAL app::update reducer. Points XDG_CONFIG_HOME at a temp
// dir and disables the keystore so the credential + registry round-trips hit
// sealed files, exactly like oauth_proactive_refresh_test.
#include "agtest.hpp"

#include "agentty/runtime/app/update.hpp"
#include "agentty/runtime/app/deps.hpp"
#include "agentty/auth/auth.hpp"
#include "agentty/auth/accounts.hpp"
#include "agentty/provider/selection.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

using namespace agentty;

namespace {

namespace acc = agentty::auth::accounts;
namespace fs  = std::filesystem;
namespace login = agentty::ui::login;
namespace msg = agentty::msg;

std::int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

void save_oauth(std::int64_t expires_at_ms, const std::string& refresh) {
    auth::cred::OAuth o;
    o.access_token  = "at-" + refresh;
    o.refresh_token = refresh;
    o.expires_at_ms = expires_at_ms;
    (void)auth::save_credentials(auth::Credentials{std::move(o)});
}

void isolate_config_dir() {
    static bool done = false;
    if (done) return;
    done = true;
    auto dir = fs::temp_directory_path() /
               ("agentty_acctsw_" + std::to_string(now_ms()));
    fs::create_directories(dir);
#if defined(_WIN32)
    _putenv_s("AGENTTY_HOME", dir.string().c_str());
    _putenv_s("AGENTTY_USE_KEYSTORE", "0");
#else
    ::setenv("AGENTTY_HOME", dir.string().c_str(), 1);
    ::setenv("AGENTTY_USE_KEYSTORE", "0", 1);
#endif
}

store::Settings g_settings;
void install_stub_deps() {
    app::install_deps(app::Deps{
        .stream         = [](auto, auto) {},
        .save_thread    = [](const auto&) {},
        .delete_thread  = [](const auto&) {},
        .load_threads   = [] { return std::vector<Thread>{}; },
        .load_thread    = [](const auto&) -> std::optional<Thread> { return std::nullopt; },
        .load_settings  = [] { return g_settings; },
        .save_settings  = [](const store::Settings& x) { g_settings = x; },
        .new_thread_id  = [] { return ThreadId{}; },
        .title_from     = [](std::string_view t) { return std::string{t}; },
        .auth           = auth::AuthHeader{auth::ApiKeyHeader{std::string{}}},
    });
}

// Register two anthropic accounts (A = fresh, B = long-expired) and leave A
// active. Returns the AccountList with the cursor pre-placed on `on_label`.
ui::login::AccountList setup_two_accounts(const std::string& on_label) {
    // B first: capture a long-EXPIRED OAuth as account "B".
    save_oauth(/*expires*/ now_ms() - 60 * 60 * 1000, "rt-B");
    acc::snapshot_active("anthropic", "B");
    // A second: a token that is still comfortably fresh, active.
    save_oauth(/*expires*/ now_ms() + 60 * 60 * 1000, "rt-A");
    acc::snapshot_active("anthropic", "A");

    ui::login::AccountList al;
    al.provider       = "anthropic";
    al.provider_label = "Anthropic";
    int want = 0;
    for (const auto& a : acc::list_for("anthropic")) {
        ui::login::AccountRow row;
        row.provider = "anthropic";
        row.label    = a.label;
        row.active   = (acc::active_label("anthropic") == a.label);
        if (a.label == on_label) want = static_cast<int>(al.rows.size());
        al.rows.push_back(std::move(row));
    }
    al.cursor = want;
    return al;
}

} // namespace

TEST_CASE("account switch refreshes a stale token") {
    isolate_config_dir();
    install_stub_deps();
    {
        provider::Selection sel;
        sel.kind = provider::Kind::Anthropic;
        provider::select(sel);
    }

    // ── Switch to the long-idle account B → kick a background refresh ──
    {
        Model m;
        m.ui.login = setup_two_accounts("B");
        auto [m2, cmd] = app::update(std::move(m),
                                     Msg{msg::LoginMsg{agentty::AccountSelect{}}});

        CHECK(m2.s.oauth_refresh_in_flight,
              "switching to a long-idle account with an expired token kicks "
              "a background refresh instead of installing the stale bearer");
        CHECK(acc::active_label("anthropic") == "B",
              "the switch still activates account B");
        CHECK(std::holds_alternative<ui::login::Closed>(m2.ui.login),
              "the account picker closes after the switch");
    }

    // ── Switch back to the fresh account A → NO refresh ──
    {
        acc::activate("anthropic", "B");   // make A the inactive target
        Model m;
        m.ui.login = setup_two_accounts("A");
        auto [m2, cmd] = app::update(std::move(m),
                                     Msg{msg::LoginMsg{agentty::AccountSelect{}}});

        CHECK(!m2.s.oauth_refresh_in_flight,
              "switching to an account whose token is still fresh does NOT "
              "trigger a refresh");
        CHECK(acc::active_label("anthropic") == "A", "switch activated A");
    }
}

TEST_CASE("re-login reuses the derived-label slot, no proliferation") {
    isolate_config_dir();
    install_stub_deps();
    {
        provider::Selection sel;
        sel.kind = provider::Kind::Anthropic;
        provider::select(sel);
    }

    // Start from a clean anthropic registry: drop any slots a prior case
    // left (this test asserts on the exact count).
    for (const auto& a : acc::list_for("anthropic"))
        acc::remove("anthropic", a.label);
    REQUIRE(acc::list_for("anthropic").empty());

    // First OAuth login → one "OAuth login" slot (label derived from the
    // credential; OAuth has no per-account identity).
    save_oauth(now_ms() + 3'600'000, "rt-1");
    acc::snapshot_active("anthropic", acc::derive_current_label("anthropic"));
    REQUIRE(acc::list_for("anthropic").size() == 1);
    CHECK(acc::derive_current_label("anthropic") == "OAuth login");
    const std::string first_secret =
        acc::get("anthropic", "OAuth login")->secret;

    // Re-login THREE more times — an expired-token refresh, or just
    // re-testing. Each derives the SAME "OAuth login" label, so the fix
    // (snapshot_active upserts by (provider,label)) UPDATES the one slot
    // in place. The pre-fix reducer suffixed "OAuth login 2/3/4", the
    // reported accumulation.
    for (int i = 2; i <= 4; ++i) {
        save_oauth(now_ms() + 3'600'000, "rt-" + std::to_string(i));
        acc::snapshot_active("anthropic", acc::derive_current_label("anthropic"));
        CHECK(acc::list_for("anthropic").size() == 1,
              "re-login must not create a new slot");
    }

    // The single slot now holds a DIFFERENT (newer) credential than the
    // first login's — the secret is a sealed blob, so compare bytes
    // rather than looking for a plaintext token.
    auto slot = acc::get("anthropic", "OAuth login");
    REQUIRE(slot.has_value());
    CHECK(slot->secret != first_secret,
          "the reused slot carries the most recent login's credential");
    CHECK(acc::active_label("anthropic") == "OAuth login", "reused slot is active");

    // A DELIBERATE second account (explicit label) still coexists — the
    // reuse rule keys on the label, so distinct labels never collide.
    save_oauth(now_ms() + 3'600'000, "rt-work");
    acc::snapshot_active("anthropic", "work laptop");
    CHECK(acc::list_for("anthropic").size() == 2,
          "a distinctly-labelled account is a separate slot");
}
