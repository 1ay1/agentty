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
#include "agentty/auth/accounts.hpp"
#include "agentty/io/clipboard.hpp"
#include "agentty/provider/chatgpt/codex_oauth.hpp"
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

    // Capture this login as a named account so it's switchable in-app. When
    // the registry already has a "default", a subsequent OAuth/key login is a
    // DIFFERENT account — register it under an incrementing name so both
    // survive and the user can flip between them without re-authing.
    {
        namespace acc = agentty::auth::accounts;
        const std::string provider = "anthropic";
        std::string base = acc::derive_current_label(provider);
        if (base.empty()) base = "account";
        std::string label = base;
        // Avoid clobbering an existing DIFFERENT account that happens to share
        // the derived label (both "OAuth login", say): suffix until unique,
        // unless a slot with this exact label already holds this same login.
        for (int n = 2; acc::get(provider, label).has_value() && n < 100; ++n)
            label = base + " " + std::to_string(n);
        acc::snapshot_active(provider, label);
    }

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
    if (auto* waiting = std::get_if<login::ChatGptWaiting>(&m.ui.login);
        waiting && waiting->cancel) {
        waiting->cancel->store(true, std::memory_order_release);
    }
    if (auto* waiting = std::get_if<login::CopilotWaiting>(&m.ui.login);
        waiting && waiting->cancel) {
        waiting->cancel->store(true, std::memory_order_release);
    }
    m.ui.login = login::Closed{};
    return done(std::move(m));
}

Step sign_out(Model m) {
    // Clear the ACTIVE provider's credentials, so "Sign out" targets whatever
    // the user is currently signed in to. ChatGPT/Codex keeps its token in a
    // separate store; Anthropic uses credentials.json. OpenAI-family API keys
    // come from env / in-app paste (saved in settings.provider_keys) — we drop
    // the pasted key so a re-auth is required.
    const auto& sel = provider::active();
    std::string what = "credentials";
    if (sel.is_copilot()) {
        provider::copilot::clear_credentials();
        what = "GitHub Copilot";
    } else if (sel.is_oauth_native()) {
        provider::chatgpt::clear_codex_credentials();
        what = "ChatGPT";
    } else if (sel.kind == provider::Kind::Anthropic) {
        auth::clear_credentials();
        what = "Anthropic";
    } else if (sel.kind == provider::Kind::OpenAI) {
        // Drop the in-app-pasted key for this endpoint; env keys are the
        // process env and can't be unset from here.
        auto settings = deps().load_settings();
        settings.provider_keys.erase(sel.openai_endpoint.label);
        deps().save_settings(settings);
        what = std::string{sel.openai_endpoint.label};
    }

    // Zero the live auth header so the very next turn can't reuse a
    // now-revoked credential, then drop the user straight into sign-in.
    agentty::app::update_auth(auth::AuthHeader{});
    m.ui.login = login::Picking{};
    m.s.status = "signed out of " + what + " — sign in to continue";
    m.s.status_until = std::chrono::steady_clock::now()
                     + std::chrono::seconds{5};
    return done(std::move(m));
}

namespace {

// Canonical registry id for the active provider's accounts. Anthropic,
// ChatGPT, and GitHub Copilot have file-backed credential stores the account
// layer can snapshot; OpenAI-family keys already switch per-endpoint via the
// picker.
std::string account_provider_id(const provider::Selection& sel) {
    if (sel.is_copilot())                 return "copilot";
    if (sel.is_chatgpt())                 return "chatgpt";
    if (sel.kind == provider::Kind::Anthropic) return "anthropic";
    return {};   // no account switching for this provider
}

// Build the AccountList state for the active provider, auto-registering the
// current live login as "default" the first time so it appears as a row.
login::AccountList build_account_list(const provider::Selection& sel) {
    namespace acc = agentty::auth::accounts;
    login::AccountList al;
    al.provider       = account_provider_id(sel);
    al.provider_label = provider::provider_display_name(sel);
    if (al.provider.empty()) return al;

    auto saved = acc::list_for(al.provider);
    if (saved.empty()) {
        // Legacy single-login: capture whatever is signed in right now under
        // a derived name so the user has a switchable, removable row.
        std::string label = acc::derive_current_label(al.provider);
        if (!label.empty() && acc::snapshot_active(al.provider, label))
            saved = acc::list_for(al.provider);
    }
    const std::string active = acc::active_label(al.provider);
    for (auto& a : saved) {
        login::AccountRow row;
        row.provider = a.provider;
        row.label    = a.label;
        row.active   = (a.label == active);
        al.rows.push_back(std::move(row));
    }
    // Land the cursor on the active row so "open, hit enter" is a no-op.
    for (int i = 0; i < static_cast<int>(al.rows.size()); ++i)
        if (al.rows[static_cast<std::size_t>(i)].active) { al.cursor = i; break; }
    return al;
}

} // namespace

Step open_accounts(Model m) {
    const auto sel = provider::active();
    auto al = build_account_list(sel);
    if (al.provider.empty()) {
        // Provider has no switchable accounts — fall back to the normal
        // sign-in / add-key flow rather than showing an empty list.
        m.ui.login = login::Picking{};
        return done(std::move(m));
    }
    m.ui.login = std::move(al);
    return done(std::move(m));
}

Step account_move(Model m, int delta) {
    if (auto* al = std::get_if<login::AccountList>(&m.ui.login)) {
        const int n = static_cast<int>(al->rows.size()) + 1;   // +1 add-new row
        if (n > 0) al->cursor = ((al->cursor + delta) % n + n) % n;
        al->confirm_remove.clear();
    }
    return done(std::move(m));
}

Step account_select(Model m) {
    auto* al = std::get_if<login::AccountList>(&m.ui.login);
    if (!al) return done(std::move(m));
    const int add_row = static_cast<int>(al->rows.size());

    // Trailing "+ Add another account…" row. Keep the continuation scoped
    // to the provider the user was managing: ChatGPT and Copilot each have
    // one native OAuth method, while Anthropic offers its API-key/OAuth choices.
    if (al->cursor >= add_row) {
        const std::string provider = al->provider;
        if (provider == "chatgpt") {
            const auto attempt_id = cmd::next_codex_login_attempt_id();
            auto cancel = std::make_shared<std::atomic_bool>(false);
            m.ui.login = login::ChatGptWaiting{
                .attempt_id = attempt_id,
                .cancel = cancel,
                .device_auth = provider::chatgpt::codex_device_auth_preferred(),
            };
            return {std::move(m), cmd::codex_login_async(attempt_id, std::move(cancel))};
        }
        if (provider == "copilot") {
            const auto attempt_id = cmd::next_codex_login_attempt_id();
            auto cancel = std::make_shared<std::atomic_bool>(false);
            m.ui.login = login::CopilotWaiting{
                .attempt_id = attempt_id,
                .cancel = cancel,
            };
            return {std::move(m), cmd::copilot_login_async(attempt_id, std::move(cancel))};
        }
        m.ui.login = login::Picking{.provider = provider};
        return done(std::move(m));
    }

    const auto& row = al->rows[static_cast<std::size_t>(al->cursor)];
    if (row.active) {                         // already this account
        m.ui.login = login::Closed{};
        return done(std::move(m));
    }

    namespace acc = agentty::auth::accounts;
    const std::string provider = row.provider;
    const std::string label    = row.label;
    if (!acc::activate(provider, label)) {
        m.ui.login = login::Failed{"could not switch to \"" + label + "\""};
        return done(std::move(m));
    }
    // Re-install the live auth header from the now-swapped active store.
    if (provider == "anthropic") {
        if (auto c = auth::load_credentials())
            agentty::app::update_auth(auth::make_auth_header(*c));
    } else if (provider == "chatgpt" || provider == "copilot") {
        // The Codex / Copilot transports read their token from the store on
        // each turn; clearing the cached header forces a fresh read next turn.
        agentty::app::update_auth(auth::AuthHeader{});
    }
    const std::string provider_label = al->provider_label;
    m.ui.login = login::Closed{};
    m.s.status = "switched " + provider_label + " to " + label;
    m.s.status_until = std::chrono::steady_clock::now()
                     + std::chrono::seconds{4};
    return done(std::move(m));
}

Step account_remove(Model m) {
    auto* al = std::get_if<login::AccountList>(&m.ui.login);
    if (!al) return done(std::move(m));
    const int add_row = static_cast<int>(al->rows.size());
    if (al->cursor >= add_row) return done(std::move(m));   // add-new row: nothing to remove

    namespace acc = agentty::auth::accounts;
    const auto row = al->rows[static_cast<std::size_t>(al->cursor)];

    // Destructive actions are deliberately two-step. A stray Backspace or
    // vim `d` must never erase a saved refresh token with no way back.
    if (al->confirm_remove != row.label) {
        al->confirm_remove = row.label;
        return done(std::move(m));
    }

    const bool was_active = row.active;
    const int old_cursor = al->cursor;
    const std::string provider_label = al->provider_label;
    acc::remove(row.provider, row.label);

    // If we removed the account we're currently authed as, the newest
    // remaining one (promoted to active by remove()) becomes live.
    if (was_active) {
        if (auto next = acc::get(row.provider, acc::active_label(row.provider))) {
            acc::activate(row.provider, next->label);
            if (row.provider == "anthropic") {
                if (auto c = auth::load_credentials())
                    agentty::app::update_auth(auth::make_auth_header(*c));
            } else {
                agentty::app::update_auth(auth::AuthHeader{});
            }
            m.s.status = "removed " + row.label + " · switched to " + next->label;
        } else {
            // The registry is empty. Clear the underlying live credential
            // file too; otherwise build_account_list() would rediscover and
            // silently resurrect the account the user just removed.
            if (row.provider == "anthropic")      auth::clear_credentials();
            else if (row.provider == "copilot")   provider::copilot::clear_credentials();
            else                                  provider::chatgpt::clear_codex_credentials();
            agentty::app::update_auth(auth::AuthHeader{});

            login::AccountList empty;
            empty.provider = row.provider;
            empty.provider_label = provider_label;
            m.ui.login = std::move(empty);  // stays on "+ Add another account…"
            m.s.status = "removed the last " + provider_label + " account";
            m.s.status_until = std::chrono::steady_clock::now()
                             + std::chrono::seconds{4};
            return done(std::move(m));
        }
    } else {
        m.s.status = "removed " + row.label;
    }

    m.s.status_until = std::chrono::steady_clock::now()
                     + std::chrono::seconds{4};

    // Rebuild the list in place and keep the cursor near the removed row.
    auto rebuilt = build_account_list(provider::active());
    rebuilt.cursor = std::min(old_cursor,
                              static_cast<int>(rebuilt.rows.size()));
    m.ui.login = std::move(rebuilt);
    return done(std::move(m));
}

Step login_pick_method(Model m, char32_t key) {
    const auto* picking = std::get_if<login::Picking>(&m.ui.login);
    if (!picking && !std::holds_alternative<login::Failed>(m.ui.login))
        return done(std::move(m));
    const bool anthropic_only = picking && picking->provider == "anthropic";
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
    if (key == U'3' && !anthropic_only) {
        // Native ChatGPT OAuth. Local terminals use the browser + loopback
        // callback; SSH terminals automatically use OpenAI device auth and
        // receive a one-time code through CodexDeviceCodeReady.
        const auto attempt_id = cmd::next_codex_login_attempt_id();
        auto cancel = std::make_shared<std::atomic_bool>(false);
        m.ui.login = login::ChatGptWaiting{
            .attempt_id = attempt_id,
            .cancel = cancel,
            .device_auth = provider::chatgpt::codex_device_auth_preferred(),
        };
        return {std::move(m), cmd::codex_login_async(attempt_id, std::move(cancel))};
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
        if (spec.empty()) {
            m.ui.login = login::Failed{"no host entered"};
            return done(std::move(m));
        }

        // Remote (TLS) custom hosts need an API key — local servers
        // (http://, bare host:port) conventionally don't. For TLS hosts,
        // hand off to the ApiKeyInput modal instead of committing immediately:
        // the user pastes a key, it's saved to provider_keys[spec], and the
        // existing ApiKeyInput arm commits the switch. Esc at the key prompt
        // dispatches CloseLogin → no switch (matching how Esc works for
        // every other login sub-state). For non-TLS hosts, fall through to
        // the keyless commit path below.
        const bool needs_key = provider::parse_selection(spec)
                                   .openai_endpoint.use_tls;
        if (needs_key) {
            std::string label = provider::provider_display_name(
                provider::parse_selection(spec));
            // Pre-fill the key field with any key already saved for this
            // spec so re-entering a known host shows its current key (masked)
            // for confirmation/edit, not a blank field.
            std::string existing_key;
            {
                auto settings = deps().load_settings();
                if (auto it = settings.provider_keys.find(spec);
                    it != settings.provider_keys.end())
                    existing_key = it->second;
            }
            // Capture size BEFORE the move: designated initializers evaluate
            // in declaration order (key_input before cursor), so
            // std::move(existing_key) into .key_input would leave
            // existing_key moved-from when .cursor reads .size().
            const int key_len = static_cast<int>(existing_key.size());
            m.ui.login = login::ApiKeyInput{
                .key_input      = std::move(existing_key),
                .cursor         = key_len,
                .provider       = spec,
                .provider_label = std::move(label),
            };
            return done(std::move(m));
        }

        // Non-TLS (local) host: no key needed. Resolve auth (will be empty
        // for local servers, which is correct — list_models only short-
        // circuits on use_tls && is_empty(auth)) and commit immediately.
        // Reuse provider_keys[spec] if present (a keyed local proxy the
        // user previously configured); otherwise the OPENAI_API_KEY chain
        // is consulted as a fallback.
        auth::AuthHeader anthropic_creds = deps().auth;
        if (auto saved = auth::load_credentials())
            anthropic_creds = auth::make_auth_header(*saved);
        std::string saved_provider_key;
        {
            auto settings = deps().load_settings();
            if (auto it = settings.provider_keys.find(spec);
                it != settings.provider_keys.end())
                saved_provider_key = it->second;
        }
        auth::AuthHeader new_auth = provider::resolve_auth_for(
            spec, anthropic_creds, /*cli_key=*/{}, saved_provider_key);
        m.ui.login = login::Closed{};
        return commit_provider_switch(std::move(m), spec, std::move(new_auth),
                                      provider::provider_display_name(
                                          provider::parse_selection(spec)));
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
    if (auto* oc = std::get_if<login::OAuthCode>(&m.ui.login)) {
        if (oc->authorize_url.empty()) return done(std::move(m));
        auto url = oc->authorize_url;
        (void)write_clipboard_text(url);   // native pbcopy/wl-copy/xclip
        auto write_cmd = Cmd<Msg>::write_clipboard(url);
        auto toast = set_status_toast(m,
            "authorize URL copied to clipboard",
            std::chrono::seconds{3});
        return {std::move(m),
            Cmd<Msg>::batch(std::move(write_cmd), std::move(toast))};
    }
    // Copilot device flow: the CODE is what the user types into the browser,
    // so copy THAT (not the URL). Terminal text-selection can't grab it — the
    // modal re-renders on every poll tick, wiping any selection — so this
    // keystroke is the reliable way to get the code onto the clipboard.
    if (auto* cw = std::get_if<login::CopilotWaiting>(&m.ui.login)) {
        if (cw->user_code.empty()) return done(std::move(m));
        auto code = cw->user_code;
        (void)write_clipboard_text(code);
        auto write_cmd = Cmd<Msg>::write_clipboard(code);
        auto toast = set_status_toast(m,
            "code " + code + " copied to clipboard",
            std::chrono::seconds{3});
        return {std::move(m),
            Cmd<Msg>::batch(std::move(write_cmd), std::move(toast))};
    }
    return done(std::move(m));
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

Step login_codex_device_code_ready(Model m, std::uint64_t attempt_id,
                                   std::string verification_url,
                                   std::string user_code) {
    auto* waiting = std::get_if<login::ChatGptWaiting>(&m.ui.login);
    if (!waiting || waiting->attempt_id != attempt_id)
        return done(std::move(m));
    waiting->device_auth = true;
    waiting->authorize_url = std::move(verification_url);
    waiting->user_code = std::move(user_code);
    return done(std::move(m));
}

Step login_codex_done(
    Model m, std::uint64_t attempt_id,
    std::expected<provider::chatgpt::CodexCredentials, auth::OAuthError> result)
{
    auto* waiting = std::get_if<login::ChatGptWaiting>(&m.ui.login);
    if (!waiting || waiting->attempt_id != attempt_id)
        return done(std::move(m));
    if (waiting->cancel)
        waiting->cancel->store(true, std::memory_order_release);
    if (!result) {
        m.ui.login = login::Failed{result.error().render()};
        return done(std::move(m));
    }
    if (!provider::chatgpt::save_codex_credentials(*result)) {
        m.ui.login = login::Failed{
            "signed in, but encrypted credentials could not be saved"};
        return done(std::move(m));
    }
    // Persistence happens only after the attempt identity check above. An
    // abandoned or superseded worker can therefore neither switch provider
    // nor overwrite the active credential store.
    m.ui.login = login::Closed{};
    m.s.status = "signed in to ChatGPT";
    m.s.status_until = std::chrono::steady_clock::now() + std::chrono::seconds{4};
    return commit_provider_switch(std::move(m), "chatgpt",
                                  auth::AuthHeader{}, "ChatGPT");
}

Step login_copilot_device_code_ready(Model m, std::uint64_t attempt_id,
                                     std::string verification_url,
                                     std::string user_code) {
    auto* waiting = std::get_if<login::CopilotWaiting>(&m.ui.login);
    if (!waiting || waiting->attempt_id != attempt_id)
        return done(std::move(m));
    waiting->authorize_url = verification_url;
    waiting->user_code = std::move(user_code);
    // Best-effort: also open the browser to the device page so the user
    // doesn't have to type the URL. Harmless if it can't (SSH/headless).
    return {std::move(m), cmd::open_browser_async(std::move(verification_url))};
}

Step login_copilot_done(
    Model m, std::uint64_t attempt_id,
    std::expected<provider::copilot::GithubToken, auth::OAuthError> result)
{
    auto* waiting = std::get_if<login::CopilotWaiting>(&m.ui.login);
    if (!waiting || waiting->attempt_id != attempt_id)
        return done(std::move(m));
    if (waiting->cancel)
        waiting->cancel->store(true, std::memory_order_release);
    if (!result) {
        m.ui.login = login::Failed{result.error().render()};
        return done(std::move(m));
    }
    // login() already persisted the GitHub token; the proxy token is
    // exchanged lazily on the first turn. Switch the active provider now.
    m.ui.login = login::Closed{};
    m.s.status = "signed in to GitHub Copilot";
    m.s.status_until = std::chrono::steady_clock::now() + std::chrono::seconds{4};
    return commit_provider_switch(std::move(m), "copilot",
                                  auth::AuthHeader{}, "GitHub Copilot");
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
        [&](SignOut)                -> Step { return sign_out(std::move(m)); },
        [&](OpenAccounts)           -> Step { return open_accounts(std::move(m)); },
        [&](AccountMove& e)         -> Step { return account_move(std::move(m), e.delta); },
        [&](AccountSelect)          -> Step { return account_select(std::move(m)); },
        [&](AccountRemove)          -> Step { return account_remove(std::move(m)); },
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
        [&](CodexDeviceCodeReady& e) -> Step {
            return login_codex_device_code_ready(std::move(m), e.attempt_id,
                std::move(e.verification_url), std::move(e.user_code));
        },
        [&](CodexLoginDone& e)      -> Step {
            return login_codex_done(std::move(m), e.attempt_id,
                                    std::move(e.result));
        },
        [&](CopilotDeviceCodeReady& e) -> Step {
            return login_copilot_device_code_ready(std::move(m), e.attempt_id,
                std::move(e.verification_url), std::move(e.user_code));
        },
        [&](CopilotLoginDone& e)    -> Step {
            return login_copilot_done(std::move(m), e.attempt_id,
                                      std::move(e.result));
        },
        [&](TokenRefreshed& e)      -> Step { return token_refreshed(std::move(m), std::move(e.result)); },
    }, lm);
}

} // namespace agentty::app::detail
