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
//   F. Device-code progress populates the waiting state.
//   G. Late device-code progress after cancellation is ignored.
//   H. A stale attempt cannot overwrite a newer attempt's code.
//   I. Closing the modal trips cooperative worker cancellation.
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
        m.ui.login = login::ChatGptWaiting{.attempt_id = 1};
        agentty::auth::OAuthError err{
            agentty::auth::OAuthErrorKind::Network, "callback timed out"};
        auto [m2, cmd] = agentty::app::detail::login_update(
            std::move(m),
            msg::LoginMsg{agentty::CodexLoginDone{
                .attempt_id = 1,
                .result = std::unexpected(err),
            }});
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
            msg::LoginMsg{agentty::CodexLoginDone{
                .attempt_id = 1,
                .result = std::unexpected(err),
            }});
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

    // F. Device-code progress updates the live waiting panel.
    {
        Model m;
        m.ui.login = login::ChatGptWaiting{
            .attempt_id = 42,
            .device_auth = true,
        };
        auto [m2, cmd] = agentty::app::detail::login_update(
            std::move(m), msg::LoginMsg{agentty::CodexDeviceCodeReady{
                .attempt_id = 42,
                .verification_url = "https://auth.openai.com/codex/device",
                .user_code = "ABCD-EFGH",
            }});
        auto* waiting = std::get_if<login::ChatGptWaiting>(&m2.ui.login);
        check(waiting && waiting->device_auth,
              "F: device-code progress remains in device mode");
        check(waiting && waiting->authorize_url.find("/codex/device") != std::string::npos,
              "F: device verification URL reaches waiting state");
        check(waiting && waiting->user_code == "ABCD-EFGH",
              "F: one-time code reaches waiting state");
        check(cmd.is_none(), "F: progress update launches no second command");
    }

    // G. A code from an abandoned worker must not reopen a cancelled modal.
    {
        Model m;
        m.ui.login = login::Closed{};
        auto [m2, cmd] = agentty::app::detail::login_update(
            std::move(m), msg::LoginMsg{agentty::CodexDeviceCodeReady{
                .attempt_id = 7,
                .verification_url = "https://auth.openai.com/codex/device",
                .user_code = "LATE-CODE",
            }});
        check(std::holds_alternative<login::Closed>(m2.ui.login),
              "G: late device code after Esc leaves modal closed");
        check(cmd.is_none(), "G: late device code launches no command");
    }

    // H. Progress from attempt A cannot alter a newer attempt B.
    {
        Model m;
        m.ui.login = login::ChatGptWaiting{
            .attempt_id = 200,
            .device_auth = true,
            .user_code = "CURRENT",
        };
        auto [m2, cmd] = agentty::app::detail::login_update(
            std::move(m), msg::LoginMsg{agentty::CodexDeviceCodeReady{
                .attempt_id = 199,
                .verification_url = "https://auth.openai.com/codex/device",
                .user_code = "STALE",
            }});
        auto* waiting = std::get_if<login::ChatGptWaiting>(&m2.ui.login);
        check(waiting && waiting->user_code == "CURRENT",
              "H: stale attempt cannot overwrite newer device code");
        check(cmd.is_none(), "H: stale progress launches no command");
    }

    // I. Closing a waiting modal trips its cooperative cancellation token.
    {
        Model m;
        auto cancel = std::make_shared<std::atomic_bool>(false);
        m.ui.login = login::ChatGptWaiting{
            .attempt_id = 300,
            .cancel = cancel,
            .device_auth = true,
        };
        auto [m2, cmd] = agentty::app::detail::login_update(
            std::move(m), msg::LoginMsg{agentty::CloseLogin{}});
        check(std::holds_alternative<login::Closed>(m2.ui.login),
              "I: Esc closes ChatGPT login modal");
        check(cancel->load(std::memory_order_acquire),
              "I: Esc cancels the active ChatGPT login worker");
        check(cmd.is_none(), "I: close launches no command");
    }

    std::printf("%d checks, %d failures\n", g_checks, g_fails);
    if (g_fails == 0) { std::printf("PASSED\n"); return 0; }
    std::printf("FAILED\n");
    return 1;
}
