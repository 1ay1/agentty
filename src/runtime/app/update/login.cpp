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

#include "agentty/runtime/app/update/internal.hpp"

#include <chrono>
#include <utility>
#include <variant>

#include <maya/core/overload.hpp>

#include "agentty/auth/auth.hpp"
#include "agentty/provider/registry.hpp"
#include "agentty/provider/selection.hpp"
#include "agentty/runtime/app/cmd_factory.hpp"
#include "agentty/runtime/app/deps.hpp"
#include "agentty/runtime/view/helpers.hpp"
#include "agentty/tool/subagent.hpp"

namespace agentty::app::detail {

using maya::Cmd;
using maya::overload;
namespace login = agentty::ui::login;

namespace {

// Persist + live-install credentials, then close the modal. Single
// point so OAuth and ApiKey paths can't drift — both end here.
void install_and_close(Model& m, auth::Credentials creds) {
    auth::save_credentials(creds);
    agentty::app::update_auth(auth::make_auth_header(creds));
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
    if (key == U'2') {
        // OAuth: mint PKCE pair, open browser, transition to OAuthCode.
        // The URL lives in state so the modal can show it as a fallback
        // if the system browser opener fails silently (broken xdg-open,
        // headless SSH session, etc.).
        //
        // random_urlsafe throws if the OpenSSL CSPRNG is unavailable
        // (astronomically rare, but a pure reducer must not propagate an
        // exception into maya's update loop). Fail closed into the login
        // modal's Failed state instead of minting a weak/empty secret.
        try {
            auth::PkceVerifier verifier{auth::random_urlsafe(128)};
            auth::OAuthState   state{auth::random_urlsafe(32)};
            std::string url = auth::oauth_authorize_url(verifier, state);
            login::OAuthCode oc;
            oc.verifier      = std::move(verifier);
            oc.state         = std::move(state);
            oc.authorize_url = url;
            m.ui.login = std::move(oc);
            return {std::move(m), cmd::open_browser_async(std::move(url))};
        } catch (const std::exception& e) {
            m.ui.login = login::Failed{
                std::string{"could not start secure login: "} + e.what()};
            return done(std::move(m));
        }
    }
    if (key == U'1') {
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
        [&](login::CustomHostInput& s) {
            s.host_input.insert(s.cursor, utf8);
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
        [](login::CustomHostInput& s) {
            if (s.cursor > 0 && !s.host_input.empty()) {
                int p = ui::utf8_prev(s.host_input, s.cursor);
                s.host_input.erase(p, s.cursor - p);
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
        [&](login::CustomHostInput& s) {
            s.host_input.insert(s.cursor, text);
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
        [](login::CustomHostInput& s) {
            s.cursor = ui::utf8_prev(s.host_input, s.cursor);
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
        [](login::CustomHostInput& s) {
            s.cursor = ui::utf8_next(s.host_input, s.cursor);
        },
        [](auto&) {},
    }, m.ui.login);
    return done(std::move(m));
}

Step login_submit(Model m) {
    if (auto* ch = std::get_if<login::CustomHostInput>(&m.ui.login)) {
        std::string spec = std::move(ch->host_input);
        while (!spec.empty() && (spec.back() == '\r' || spec.back() == '\n'
                               || spec.back() == ' ' || spec.back() == '\t'))
            spec.pop_back();
        // Strip a leading scheme if the user pasted a URL — from_spec wants
        // a bare host[:port].
        for (std::string_view pfx : {"http://", "https://"})
            if (spec.rfind(pfx, 0) == 0) { spec.erase(0, pfx.size()); break; }
        // Trim a trailing slash / path — only host[:port] is meaningful.
        if (auto slash = spec.find('/'); slash != std::string::npos)
            spec.erase(slash);
        if (spec.empty()) {
            m.ui.login = login::Failed{"no host entered"};
            return done(std::move(m));
        }

        // Custom hosts are treated as keyless local/compatible endpoints;
        // resolve_auth_for still consults the OPENAI_API_KEY chain in case
        // the user is fronting a keyed proxy. Load Anthropic creds fresh from
        // disk (NOT deps().auth) so resolve_auth_for never echoes the current
        // provider's empty key — same footgun the picker guards against.
        auth::AuthHeader anthropic_creds = deps().auth;
        if (auto saved = auth::load_credentials())
            anthropic_creds = auth::make_auth_header(*saved);
        auth::AuthHeader new_auth =
            provider::resolve_auth_for(spec, anthropic_creds);
        m.ui.login = login::Closed{};
        return commit_provider_switch(std::move(m), spec, std::move(new_auth), spec);
    }
    if (auto* api = std::get_if<login::ApiKeyInput>(&m.ui.login)) {
        std::string key = std::move(api->key_input);
        const std::string provider = api->provider;
        const std::string provider_label = api->provider_label;
        // Trim trailing whitespace — paste handlers may include a stray
        // newline depending on terminal pasting behaviour.
        while (!key.empty() && (key.back() == '\r' || key.back() == '\n'
                              || key.back() == ' ' || key.back() == '\t'))
            key.pop_back();
        if (key.empty()) {
            m.ui.login = login::Failed{"no key entered"};
            return done(std::move(m));
        }

        // OpenAI-family key: persist under Settings.provider_keys[id], then
        // commit the live provider switch the picker deferred. The Anthropic
        // path (empty provider) keeps using credentials.json below.
        if (!provider.empty()) {
            {
                auto settings = deps().load_settings();
                settings.provider_keys[provider] = key;
                settings.provider = provider;
                deps().save_settings(settings);
            }
            // Build the new backend's auth from the just-pasted key (it isn't
            // in deps().load_settings()'s in-memory copy used elsewhere; pass
            // it as the saved_key so the resolver picks it without a reload).
            auth::AuthHeader new_auth = provider::resolve_auth_for(
                provider, deps().auth, /*cli_key=*/{}, /*saved_key=*/key);
            m.ui.login = login::Closed{};
            // The saved key persists across the helper's load-modify-save
            // (persist_settings preserves provider_keys), so the switch is
            // committed through the ONE shared path like every other entry.
            return commit_provider_switch(std::move(m), provider,
                                          std::move(new_auth), provider_label);
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

Step login_copy_auth_url(Model m) {
    auto* oc = std::get_if<login::OAuthCode>(&m.ui.login);
    if (!oc || oc->authorize_url.empty()) return done(std::move(m));
    auto url = oc->authorize_url;
    auto write_cmd = Cmd<Msg>::write_clipboard(url);
    auto toast = set_status_toast(m,
        "authorize URL copied to clipboard",
        std::chrono::seconds{3});
    return {std::move(m),
        Cmd<Msg>::batch(std::move(write_cmd), std::move(toast))};
}

Step login_open_browser_again(Model m) {
    auto* oc = std::get_if<login::OAuthCode>(&m.ui.login);
    if (!oc || oc->authorize_url.empty()) return done(std::move(m));
    auto url = oc->authorize_url;
    auto open_cmd = cmd::open_browser_async(std::move(url));
    auto toast = set_status_toast(m,
        "opening browser\xe2\x80\xa6",
        std::chrono::seconds{2});
    return {std::move(m),
        Cmd<Msg>::batch(std::move(open_cmd), std::move(toast))};
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
    // path was kicked off either by `init()` (stale-but-refreshable
    // token on disk) or by the StreamError handler reacting to a
    // mid-session 401 (see stream.cpp, ErrorClass::Auth branch). The
    // modal state doesn't change here either way; the stream ctx
    // parked in retry::Scheduled is what tells us we owe a RetryStream.
    m.s.oauth_refresh_in_flight = false;

    // Was a stream parked waiting for this refresh? If so, we either
    // resume it (success) or tear it down to Idle (failure). Detected
    // structurally via retry::Scheduled on the active ctx — the only
    // way the phase reaches that state without a RetryStream already
    // in flight is the auth-refresh branch in stream.cpp.
    const bool stream_parked = m.s.in_scheduled();

    if (!result) {
        // Refresh failed — surface the typed error in the bottom row.
        // The "error:" prefix triggers shortcut_row.cpp's danger
        // styling. 6s gives the user time to read before the toast
        // expires; the Cmd::after sentinel auto-clears so a later
        // status write doesn't get pre-empted.
        std::string text = std::string{"error: token refresh failed: "}
                         + result.error().render();

        // If a stream was parked on this refresh, tear it down: there's
        // no fresh token coming, so retrying would just 401 again.
        // Drop to Idle and finalise any in-flight tool calls so the
        // session is cleanly recoverable via the login modal.
        if (stream_parked) {
            auto now = std::chrono::steady_clock::now();
            if (!m.d.current.messages.empty()
                && m.d.current.messages.back().role == Role::Assistant) {
                auto& last = m.d.current.messages.back();
                last.error = text;
                for (auto& tc : last.tool_calls) {
                    if (!tc.is_terminal()) {
                        tc.status = ToolUse::Failed{
                            tc.started_at(), now,
                            "auth refresh failed"};
                    }
                    std::string{}.swap(tc.args_streaming);
                }
                if (last.text.empty() && last.tool_calls.empty()) {
                    m.d.current.messages.pop_back();
                }
            }
            m.s.phase = phase::Idle{};
        }

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
    agentty::app::update_auth(auth::make_auth_header(creds));

    auto toast_cmd = set_status_toast(m, "OAuth token refreshed",
                                      std::chrono::seconds{3});

    // A stream was parked waiting for this refresh (the StreamError
    // handler's Auth branch left the phase in Streaming{retry::Scheduled}).
    // Resume by dispatching RetryStream — the existing RetryStream arm
    // flips retry back to Fresh and calls launch_stream, which picks
    // up the freshly-installed bearer from Deps.
    if (stream_parked) {
        return {std::move(m),
            Cmd<Msg>::batch(std::vector<Cmd<Msg>>{
                std::move(toast_cmd),
                Cmd<Msg>::after(std::chrono::milliseconds{0},
                                Msg{RetryStream{}})})};
    }

    // Drain any text the user queued while the refresh was in flight.
    // Mirrors the stream-finish drain at update/stream.cpp:617 — pull
    // the front off `composer.queued`, hand it to submit_message, and
    // batch its Cmd alongside the toast so the user's first turn fires
    // the moment fresh creds are live.
    if (m.s.is_idle() && !m.ui.composer.queued.empty()) {
        auto& head = m.ui.composer.queued.front();
        m.ui.composer.text        = std::move(head.text);
        m.ui.composer.attachments = std::move(head.attachments);
        m.ui.composer.cursor      = static_cast<int>(m.ui.composer.text.size());
        m.ui.composer.queued.erase(m.ui.composer.queued.begin());
        auto [mm, sub_cmd] = submit_message(std::move(m));
        m = std::move(mm);
        return {std::move(m),
            Cmd<Msg>::batch(std::vector<Cmd<Msg>>{
                std::move(toast_cmd), std::move(sub_cmd)})};
    }
    return {std::move(m), std::move(toast_cmd)};
}

// ============================================================================
// login_update — reducer for `msg::LoginMsg`
// ============================================================================
// Thin dispatch over the per-arm helpers above; the typed state-machine
// guarantees the helpers see a modal in the right state.

Step login_update(Model m, msg::LoginMsg lm) {
    return std::visit(overload{
        [&](OpenLogin)              -> Step { return open_login(std::move(m)); },
        [&](CloseLogin)             -> Step { return close_login(std::move(m)); },
        [&](LoginPickMethod& e)     -> Step { return login_pick_method(std::move(m), e.key); },
        [&](LoginCharInput& e)      -> Step { return login_char_input(std::move(m), e.ch); },
        [&](LoginBackspace)         -> Step { return login_backspace(std::move(m)); },
        [&](LoginPaste& e)          -> Step { return login_paste(std::move(m), std::move(e.text)); },
        [&](LoginCursorLeft)        -> Step { return login_cursor_left(std::move(m)); },
        [&](LoginCursorRight)       -> Step { return login_cursor_right(std::move(m)); },
        [&](LoginSubmit)            -> Step { return login_submit(std::move(m)); },
        [&](LoginCopyAuthUrl)       -> Step { return login_copy_auth_url(std::move(m)); },
        [&](LoginOpenBrowserAgain)  -> Step { return login_open_browser_again(std::move(m)); },
        [&](LoginExchanged& e)      -> Step { return login_exchanged(std::move(m), std::move(e.result)); },
        [&](TokenRefreshed& e)      -> Step { return token_refreshed(std::move(m), std::move(e.result)); },
    }, lm);
}

} // namespace agentty::app::detail
