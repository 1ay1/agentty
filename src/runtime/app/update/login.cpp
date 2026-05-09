// In-app login modal reducer arms. Lives outside update.cpp because the
// OAuth flow drags in `auth/auth.hpp` + `cmd_factory.hpp` worth of
// dependencies that the rest of update.cpp doesn't need.
//
// The modal is a closed sum (`ui::login::State`): Closed | Picking |
// OAuthCode | OAuthExchanging | ApiKeyInput | Failed. Every arm here
// either dispatches via `std::visit` into the active alternative or
// short-circuits when the modal isn't in a state that accepts the Msg —
// the typed state machine is what guarantees we never read OAuthCode
// fields from an ApiKeyInput modal, etc.

#include "moha/runtime/app/update/internal.hpp"

#include <chrono>
#include <utility>
#include <variant>

#include <maya/core/overload.hpp>

#include "moha/auth/auth.hpp"
#include "moha/runtime/app/cmd_factory.hpp"
#include "moha/runtime/app/deps.hpp"
#include "moha/runtime/view/helpers.hpp"

namespace moha::app::detail {

using maya::Cmd;
using maya::overload;
namespace login = moha::ui::login;

namespace {

// Persist + live-install credentials, then close the modal. Single
// point so OAuth and ApiKey paths can't drift — both end here.
void install_and_close(Model& m, auth::Credentials creds) {
    auth::save_credentials(creds);
    moha::app::update_auth(auth::header_value(creds), auth::style(creds));
    m.ui.login = login::Closed{};
    m.s.status = "logged in";
    m.s.status_until = std::chrono::steady_clock::now()
                     + std::chrono::seconds{4};
}

} // namespace

Step open_login(Model m) {
    m.ui.login = login::Picking{};
    return done(std::move(m));
}

Step close_login(Model m) {
    m.ui.login = login::Closed{};
    return done(std::move(m));
}

Step login_pick_method(Model m, char32_t key) {
    if (!std::holds_alternative<login::Picking>(m.ui.login)
        && !std::holds_alternative<login::Failed>(m.ui.login))
        return done(std::move(m));
    if (key == U'1') {
        // OAuth: mint PKCE pair, open browser, transition to OAuthCode.
        // The URL lives in state so the modal can show it as a fallback
        // if the system browser opener fails silently (broken xdg-open,
        // headless SSH session, etc.).
        auth::PkceVerifier verifier{auth::random_urlsafe(128)};
        auth::OAuthState   state{auth::random_urlsafe(32)};
        std::string url = auth::oauth_authorize_url(verifier, state);
        login::OAuthCode oc;
        oc.verifier      = std::move(verifier);
        oc.state         = std::move(state);
        oc.authorize_url = url;
        m.ui.login = std::move(oc);
        return {std::move(m), cmd::open_browser_async(std::move(url))};
    }
    if (key == U'2') {
        m.ui.login = login::ApiKeyInput{};
        return done(std::move(m));
    }
    return done(std::move(m));
}

Step login_char_input(Model m, char32_t ch) {
    auto utf8 = ui::utf8_encode(ch);
    std::visit(overload{
        [&](login::OAuthCode& s) {
            s.code_input.insert(s.cursor, utf8);
            s.cursor += static_cast<int>(utf8.size());
        },
        [&](login::ApiKeyInput& s) {
            s.key_input.insert(s.cursor, utf8);
            s.cursor += static_cast<int>(utf8.size());
        },
        [](auto&) {},
    }, m.ui.login);
    return done(std::move(m));
}

Step login_backspace(Model m) {
    std::visit(overload{
        [](login::OAuthCode& s) {
            if (s.cursor > 0 && !s.code_input.empty()) {
                int p = ui::utf8_prev(s.code_input, s.cursor);
                s.code_input.erase(p, s.cursor - p);
                s.cursor = p;
            }
        },
        [](login::ApiKeyInput& s) {
            if (s.cursor > 0 && !s.key_input.empty()) {
                int p = ui::utf8_prev(s.key_input, s.cursor);
                s.key_input.erase(p, s.cursor - p);
                s.cursor = p;
            }
        },
        [](auto&) {},
    }, m.ui.login);
    return done(std::move(m));
}

Step login_paste(Model m, std::string text) {
    std::visit(overload{
        [&](login::OAuthCode& s) {
            s.code_input.insert(s.cursor, text);
            s.cursor += static_cast<int>(text.size());
        },
        [&](login::ApiKeyInput& s) {
            s.key_input.insert(s.cursor, text);
            s.cursor += static_cast<int>(text.size());
        },
        [](auto&) {},
    }, m.ui.login);
    return done(std::move(m));
}

Step login_cursor_left(Model m) {
    std::visit(overload{
        [](login::OAuthCode& s) {
            s.cursor = ui::utf8_prev(s.code_input, s.cursor);
        },
        [](login::ApiKeyInput& s) {
            s.cursor = ui::utf8_prev(s.key_input, s.cursor);
        },
        [](auto&) {},
    }, m.ui.login);
    return done(std::move(m));
}

Step login_cursor_right(Model m) {
    std::visit(overload{
        [](login::OAuthCode& s) {
            s.cursor = ui::utf8_next(s.code_input, s.cursor);
        },
        [](login::ApiKeyInput& s) {
            s.cursor = ui::utf8_next(s.key_input, s.cursor);
        },
        [](auto&) {},
    }, m.ui.login);
    return done(std::move(m));
}

Step login_submit(Model m) {
    if (auto* api = std::get_if<login::ApiKeyInput>(&m.ui.login)) {
        std::string key = std::move(api->key_input);
        // Trim trailing whitespace — paste handlers may include a stray
        // newline depending on terminal pasting behaviour.
        while (!key.empty() && (key.back() == '\r' || key.back() == '\n'
                              || key.back() == ' ' || key.back() == '\t'))
            key.pop_back();
        if (key.empty()) {
            m.ui.login = login::Failed{"no key entered"};
            return done(std::move(m));
        }
        install_and_close(m, auth::Credentials{auth::cred::ApiKey{std::move(key)}});
        return done(std::move(m));
    }
    if (auto* oc = std::get_if<login::OAuthCode>(&m.ui.login)) {
        std::string code_raw = std::move(oc->code_input);
        while (!code_raw.empty() && (code_raw.back() == '\r' || code_raw.back() == '\n'
                                   || code_raw.back() == ' ' || code_raw.back() == '\t'))
            code_raw.pop_back();
        if (code_raw.empty()) {
            // Stay in OAuthCode — leaving the verifier intact so the user
            // can re-paste without reopening the browser.
            return done(std::move(m));
        }
        auto verifier = std::move(oc->verifier);
        auto state    = std::move(oc->state);
        m.ui.login = login::OAuthExchanging{};
        return {std::move(m),
            cmd::oauth_exchange(auth::OAuthCode{std::move(code_raw)},
                                std::move(verifier), std::move(state))};
    }
    return done(std::move(m));
}

Step login_exchanged(Model m, auth::TokenResult result) {
    if (!std::holds_alternative<login::OAuthExchanging>(m.ui.login))
        return done(std::move(m));
    if (!result) {
        m.ui.login = login::Failed{result.error().render()};
        return done(std::move(m));
    }
    auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    auto& tok = *result;
    install_and_close(m, auth::Credentials{auth::cred::OAuth{
        std::move(tok.access_token),
        std::move(tok.refresh_token),
        tok.expires_in_s ? now_ms + tok.expires_in_s * 1000 : 0,
    }});
    return done(std::move(m));
}

Step token_refreshed(Model m, auth::TokenResult result) {
    // Background-refresh result. Distinct from login_exchanged: this
    // path was kicked off by `init()` for a stale-but-refreshable token
    // on disk, not by a user-driven login flow, so the modal state
    // doesn't change here.
    m.s.oauth_refresh_in_flight = false;

    if (!result) {
        // Refresh failed — surface the typed error in the bottom row.
        // The "error:" prefix triggers shortcut_row.cpp's danger
        // styling. 6s gives the user time to read before the toast
        // expires; the Cmd::after sentinel auto-clears so a later
        // status write doesn't get pre-empted.
        std::string text = std::string{"error: token refresh failed: "}
                         + result.error().render();
        auto cmd = set_status_toast(m, std::move(text),
                                    std::chrono::seconds{6});
        // Leave any queued composer text alone — the user can resubmit
        // (after re-authenticating via the login modal) without
        // retyping. The first manual send in that state will hit the
        // stale-token 401 path, but the in-app login modal is the
        // recovery surface.
        return {std::move(m), std::move(cmd)};
    }

    // Refresh OK — install fresh creds into Deps so the next stream
    // uses the new bearer, persist them so a relaunch doesn't refresh
    // again, and surface a success toast.
    auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    auto& tok = *result;
    auth::Credentials creds{auth::cred::OAuth{
        std::move(tok.access_token),
        std::move(tok.refresh_token),
        tok.expires_in_s ? now_ms + tok.expires_in_s * 1000 : 0,
    }};
    auth::save_credentials(creds);
    moha::app::update_auth(auth::header_value(creds), auth::style(creds));

    auto toast_cmd = set_status_toast(m, "OAuth token refreshed",
                                      std::chrono::seconds{3});

    // Drain any text the user queued while the refresh was in flight.
    // Mirrors the stream-finish drain at update/stream.cpp:617 — pull
    // the front off `composer.queued`, hand it to submit_message, and
    // batch its Cmd alongside the toast so the user's first turn fires
    // the moment fresh creds are live.
    if (m.s.is_idle() && !m.ui.composer.queued.empty()) {
        m.ui.composer.text = m.ui.composer.queued.front();
        m.ui.composer.queued.erase(m.ui.composer.queued.begin());
        auto [mm, sub_cmd] = submit_message(std::move(m));
        m = std::move(mm);
        return {std::move(m),
            Cmd<Msg>::batch(std::vector<Cmd<Msg>>{
                std::move(toast_cmd), std::move(sub_cmd)})};
    }
    return {std::move(m), std::move(toast_cmd)};
}

} // namespace moha::app::detail
