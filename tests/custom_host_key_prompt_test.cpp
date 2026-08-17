// custom_host_key_prompt_test — the CustomHostInput → ApiKeyInput handoff for
// TLS custom hosts. A remote https://host/path needs an API key before the
// provider switch commits; a local http://host:port or bare host:port commits
// keyless as before. Esc at the key prompt cancels the whole switch.
//
// Pure reducer test: stubs deps() with an in-memory store::Settings and
// sets XDG_CONFIG_HOME to a temp dir so auth::load_credentials() returns
// nullopt (no on-disk creds interfere with the non-TLS path). The test
// detects whether commit_provider_switch ran via m.s.models_loading (which
// commit_provider_switch sets at modal.cpp:691) — same indirection
// codex_login_flow_test uses for the ChatGPT path.
//
// Run: build the `custom_host_key_prompt_test` target, execute. Exit 0 = pass.

#include "agentty/runtime/app/update/internal.hpp"
#include "agentty/runtime/app/deps.hpp"
#include "agentty/runtime/login.hpp"
#include "agentty/runtime/model.hpp"
#include "agentty/runtime/msg.hpp"
#include "agentty/store/store.hpp"
#include "agentty/provider/selection.hpp"

#include <cstdlib>
#include <filesystem>
#include <string>

#include "agtest.hpp"

namespace fs = std::filesystem;
namespace login = agentty::ui::login;
namespace msg = agentty::msg;
namespace app = agentty::app;

// Helper: install deps() with an in-memory Settings captured by reference.
// The other Deps members are stubbed to no-ops since login_submit's
// CustomHostInput arm only calls deps().load_settings() and deps().auth.
static void install_stub_deps(agentty::store::Settings& s) {
    app::install_deps(app::Deps{
        .stream         = [](auto, auto) {},
        .save_thread    = [](const auto&) {},
        .load_threads   = [] { return std::vector<agentty::Thread>{}; },
        .load_thread    = [](const auto&) -> std::optional<agentty::Thread> { return std::nullopt; },
        .load_settings  = [&s] { return s; },
        .save_settings  = [&s](const agentty::store::Settings& x) { s = x; },
        .new_thread_id  = [] { return agentty::ThreadId{}; },
        .title_from     = [](std::string_view t) { return std::string{t}; },
        .auth           = agentty::auth::AuthHeader{agentty::auth::ApiKeyHeader{std::string{}}},
    });
}

TEST_CASE("custom host key prompt transitions") {

    // Redirect XDG_CONFIG_HOME to a temp dir with no agentty/ subdir so
    // auth::load_credentials() returns nullopt (no credentials.json exists).
    // This keeps the non-TLS path deterministic regardless of the host env.
    fs::path tmp_xdg = fs::temp_directory_path() /
        ("agentty-test-" + std::to_string(::getpid()));
    std::error_code mkdir_ec;
    fs::create_directories(tmp_xdg, mkdir_ec);
    ::setenv("XDG_CONFIG_HOME", tmp_xdg.c_str(), 1);
    // Ensure parse_selection's custom_auth_header() read is deterministic.
    agentty::provider::set_custom_auth_header("");

    // ── Case 1: TLS host, no saved key → ApiKeyInput with empty key field ──
    {
        agentty::store::Settings s;
        install_stub_deps(s);
        agentty::Model m;
        m.ui.login = login::CustomHostInput{.host_input = "https://chat.example.org/api"};
        auto [m2, cmd] = app::detail::login_update(
            std::move(m), msg::LoginMsg{agentty::LoginSubmit{}});
        auto* api = std::get_if<login::ApiKeyInput>(&m2.ui.login);
        check(api != nullptr,
              "1: TLS host transitions to ApiKeyInput (not committed)");
        check(api && api->provider == "https://chat.example.org/api",
              "1: ApiKeyInput.provider is the full URL spec (persistence key)");
        check(api && api->provider_label == "chat.example.org",
              "1: ApiKeyInput.provider_label is the host-only display name");
        check(api && api->key_input.empty(),
              "1: key_input is empty (no saved key to pre-fill)");
        check(api && api->cursor == 0,
              "1: cursor at 0 for empty key field");
        // commit_provider_switch did NOT run: models_loading stays false.
        check(!m2.s.models_loading,
              "1: no commit (models_loading false)");
        (void)cmd;
    }

    // ── Case 2: TLS host, saved key exists → ApiKeyInput pre-filled ──
    {
        agentty::store::Settings s;
        s.provider_keys["https://chat.example.org/api"] = "sk-test-key";
        install_stub_deps(s);
        agentty::Model m;
        m.ui.login = login::CustomHostInput{.host_input = "https://chat.example.org/api"};
        auto [m2, cmd] = app::detail::login_update(
            std::move(m), msg::LoginMsg{agentty::LoginSubmit{}});
        auto* api = std::get_if<login::ApiKeyInput>(&m2.ui.login);
        check(api != nullptr,
              "2: TLS host with saved key still transitions to ApiKeyInput");
        check(api && api->key_input == "sk-test-key",
              "2: key_input pre-filled with the saved key (2a)");
        check(api && api->cursor == 11,
              "2: cursor at end of pre-filled key (sk-test-key = 11 chars)");
        check(!m2.s.models_loading,
              "2: no commit (waiting for key submit)");
        (void)cmd;
    }

    // ── Case 3: Non-TLS bare host:port → commit immediately, no key prompt ──
    {
        agentty::store::Settings s;
        install_stub_deps(s);
        agentty::Model m;
        // Seed available_models so we can detect it being cleared by commit.
        m.d.available_models.push_back(agentty::ModelInfo{
            .id = agentty::ModelId{"old-model"}, .display_name = "old"});
        m.ui.login = login::CustomHostInput{.host_input = "localhost:8080"};
        auto [m2, cmd] = app::detail::login_update(
            std::move(m), msg::LoginMsg{agentty::LoginSubmit{}});
        check(std::holds_alternative<login::Closed>(m2.ui.login),
              "3: non-TLS host commits (modal closes, not ApiKeyInput)");
        check(m2.s.models_loading,
              "3: commit_provider_switch ran (models_loading true)");
        check(m2.d.available_models.empty(),
              "3: commit_provider_switch cleared available_models");
        (void)cmd;
    }

    // ── Case 4: Non-TLS http://host:port/path → commit, no key prompt ──
    {
        agentty::store::Settings s;
        install_stub_deps(s);
        agentty::Model m;
        m.d.available_models.push_back(agentty::ModelInfo{
            .id = agentty::ModelId{"old-model"}, .display_name = "old"});
        m.ui.login = login::CustomHostInput{.host_input = "http://10.0.0.5:5000/custom"};
        auto [m2, cmd] = app::detail::login_update(
            std::move(m), msg::LoginMsg{agentty::LoginSubmit{}});
        check(std::holds_alternative<login::Closed>(m2.ui.login),
              "4: http:// host commits (modal closes)");
        check(m2.s.models_loading,
              "4: commit_provider_switch ran (models_loading true)");
        check(m2.d.available_models.empty(),
              "4: commit_provider_switch cleared available_models");
        (void)cmd;
    }

    // ── Case 5: Esc at the key prompt → cancel the whole switch ──
    {
        agentty::store::Settings s;
        install_stub_deps(s);
        agentty::Model m;
        // First submit a TLS host to land in ApiKeyInput.
        m.ui.login = login::CustomHostInput{.host_input = "https://chat.example.org/api"};
        auto [m1, cmd1] = app::detail::login_update(
            std::move(m), msg::LoginMsg{agentty::LoginSubmit{}});
        check(std::holds_alternative<login::ApiKeyInput>(m1.ui.login),
              "5: setup — TLS host entered ApiKeyInput");
        // Now press Esc (CloseLogin).
        auto [m2, cmd2] = app::detail::login_update(
            std::move(m1), msg::LoginMsg{agentty::CloseLogin{}});
        check(std::holds_alternative<login::Closed>(m2.ui.login),
              "5: Esc at key prompt closes the modal");
        check(!m2.s.models_loading,
              "5: no commit happened (models_loading false)");
        (void)cmd1; (void)cmd2;
    }

    // Cleanup the temp XDG dir.
    std::error_code ec;
    fs::remove_all(tmp_xdg, ec);
}
