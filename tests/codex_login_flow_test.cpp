// codex_login_flow_test — the in-app ChatGPT (Codex) login reducer arms that
// don't need network I/O.
//
// Locks the pure transitions of the first-class in-app ChatGPT sign-in:
//
//   A. LoginPickMethod{'3'} from the general Picking modal → ChatGptWaiting
//      (and a non-none Cmd: the async loopback login is launched).
//   B. CodexLoginDone{error} while ChatGptWaiting → Failed{message}.
//   C. CodexLoginDone arriving when the modal is NOT ChatGptWaiting (user
//      Esc'd out) is dropped — no state change.
//   D. Anthropic-scoped add-account Picking ignores ChatGPT key '3'.
//   E. The ChatGPT account list's add row launches OAuth directly.
//
// The success path (CodexLoginDone{creds}) runs commit_provider_switch, which
// needs a fully-installed Deps + process-global provider registry; it's
// covered by the live provider-switch tests, not here.

#include <cstdio>
#include <string>

#include "agentty/runtime/app/update/internal.hpp"
#include "agentty/runtime/login.hpp"
#include "agentty/runtime/model.hpp"
#include "agentty/runtime/msg.hpp"

using agentty::Model;
namespace login = agentty::ui::login;
namespace msg = agentty::msg;

static int g_checks = 0;
static int g_fails  = 0;
static void check(bool ok, const char* what) {
    ++g_checks;
    if (!ok) { ++g_fails; std::printf("FAIL: %s\n", what); }
    else     { std::printf("ok:   %s\n", what); }
}

int main() {
    std::printf("codex_login_flow_test\n");

    // A. Picking + key '3' → ChatGptWaiting, with a launch Cmd.
    {
        Model m;
        m.ui.login = login::Picking{};
        auto [m2, cmd] = agentty::app::detail::login_update(
            std::move(m), msg::LoginMsg{agentty::LoginPickMethod{U'3'}});
        check(std::holds_alternative<login::ChatGptWaiting>(m2.ui.login),
              "A: '3' from Picking enters ChatGptWaiting");
        check(!cmd.is_none(),
              "A: the async ChatGPT login Cmd is launched");
    }

    // B. A failed CodexLoginDone while waiting → Failed with the message.
    {
        Model m;
        m.ui.login = login::ChatGptWaiting{};
        agentty::auth::OAuthError err{
            agentty::auth::OAuthErrorKind::Network, "callback timed out"};
        auto [m2, cmd] = agentty::app::detail::login_update(
            std::move(m),
            msg::LoginMsg{agentty::CodexLoginDone{std::unexpected(err)}});
        auto* failed = std::get_if<login::Failed>(&m2.ui.login);
        check(failed != nullptr, "B: failure enters Failed state");
        check(failed && failed->message.find("timed out") != std::string::npos,
              "B: Failed carries the rendered error message");
        (void)cmd;
    }

    // C. A late CodexLoginDone when the modal isn't ChatGptWaiting is dropped.
    {
        Model m;
        m.ui.login = login::Closed{};
        agentty::auth::OAuthError err{
            agentty::auth::OAuthErrorKind::Network, "late arrival"};
        auto [m2, cmd] = agentty::app::detail::login_update(
            std::move(m),
            msg::LoginMsg{agentty::CodexLoginDone{std::unexpected(err)}});
        check(std::holds_alternative<login::Closed>(m2.ui.login),
              "C: a late result after Esc leaves the modal Closed");
        (void)cmd;
    }

    // D. An Anthropic-scoped add flow must not escape into ChatGPT OAuth.
    {
        Model m;
        m.ui.login = login::Picking{.provider = "anthropic"};
        auto [m2, cmd] = agentty::app::detail::login_update(
            std::move(m), msg::LoginMsg{agentty::LoginPickMethod{U'3'}});
        auto* picking = std::get_if<login::Picking>(&m2.ui.login);
        check(picking && picking->provider == "anthropic",
              "D: Anthropic add flow ignores ChatGPT choice");
        check(cmd.is_none(), "D: no ChatGPT OAuth command is launched");
    }

    // E. ChatGPT has only one add method, so its add row starts OAuth directly.
    {
        Model m;
        login::AccountList accounts;
        accounts.provider = "chatgpt";
        accounts.provider_label = "ChatGPT";
        accounts.cursor = 0; // empty list: row zero is "+ Add another account…"
        m.ui.login = std::move(accounts);
        auto [m2, cmd] = agentty::app::detail::login_update(
            std::move(m), msg::LoginMsg{agentty::AccountSelect{}});
        check(std::holds_alternative<login::ChatGptWaiting>(m2.ui.login),
              "E: ChatGPT add row enters ChatGptWaiting directly");
        check(!cmd.is_none(), "E: ChatGPT add row launches OAuth command");
    }

    std::printf("%d checks, %d failures\n", g_checks, g_fails);
    if (g_fails == 0) { std::printf("PASSED\n"); return 0; }
    std::printf("FAILED\n");
    return 1;
}
