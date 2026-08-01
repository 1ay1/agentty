#pragma once
// agentty::ui::login — the in-app authentication modal's state machine.
//
// Same shape as the other picker variants in `runtime/picker.hpp`: a
// closed sum type so the validity of each state's data is enforced
// by the type system rather than by hand-maintained invariants.
//
// Five states cover the full flow:
//
//   Closed         — modal not shown.
//   Picking        — choose OAuth (1) or paste API key (2).
//   OAuthCode      — browser opened; user is pasting the callback code.
//                    Carries the PKCE verifier + state needed to
//                    exchange the code on submit.
//   OAuthExchanging — code submitted; HTTP POST to /oauth/token in flight.
//   ApiKeyInput    — user is typing an `sk-ant-...` key.
//   CustomHostInput — user is typing a raw `host[:port]` for an
//                    OpenAI-compatible backend (llama.cpp, vLLM, a
//                    remote box). No auth; submit switches the provider
//                    to that endpoint directly.
//   Failed         — error toast; press any key to return to Picking.

#include <string>
#include <variant>
#include <vector>

#include "agentty/auth/auth.hpp"

namespace agentty::ui::login {

struct Closed {};

struct Picking {};

struct OAuthCode {
    agentty::auth::PkceVerifier verifier;
    agentty::auth::OAuthState   state;
    std::string              authorize_url;   // shown to the user as a fallback
    std::string              code_input;
    int                      cursor = 0;
};

struct OAuthExchanging {};

// Native ChatGPT (Codex) login is in flight. The browser is open and the
// loopback callback server (port 1455) is waiting for the redirect; this
// state shows a "waiting for the browser…" panel. No user input is needed
// — codex_login() drives the whole handshake and lands CodexLoginDone. Esc
// aborts (the task is best-effort abandoned; the timeout bounds it).
struct ChatGptWaiting {
    std::string authorize_url;   // shown as a copy/open-again fallback
};

struct ApiKeyInput {
    std::string key_input;
    int         cursor = 0;
    // Which backend this key is for. Empty = Anthropic (saved to
    // credentials.json). A provider id ("openai", "groq", …) routes the
    // submit to Settings.provider_keys + a live provider switch. Carries
    // the human label for the panel header so the view needs no registry
    // lookup.
    std::string provider;        // canonical id; empty = Anthropic
    std::string provider_label;  // display name for the panel title
};

// Free-text entry of a raw OpenAI-compatible endpoint ("host" or
// "host:port"). Opened from the provider picker's "Custom host…" row.
// Submit routes through provider::parse_selection(raw) — the same path
// --provider host:port takes — so any llama.cpp / vLLM / remote server
// is reachable from the UI without touching the CLI.
struct CustomHostInput {
    std::string host_input;
    int         cursor = 0;
};

struct Failed {
    std::string message;
};

// One row in the account switcher.
struct AccountRow {
    std::string provider;    // canonical id
    std::string label;       // user-facing name
    bool        active = false;
};

// The in-app account switcher: lists every saved account for the ACTIVE
// provider so the user can switch who they're signed in as — or add a new
// one / remove one — without ever leaving agentty. Selecting a row that
// isn't the active one swaps that account's credential into the live store;
// the last row is always "+ Add another account…" which drops into the
// normal Picking flow, tagged so the resulting login is snapshotted under a
// fresh name.
struct AccountList {
    std::string             provider;       // provider these rows belong to
    std::string             provider_label; // display name for the header
    std::vector<AccountRow> rows;           // saved accounts (+ synthesized add row is index == rows.size())
    int                     cursor = 0;      // 0..rows.size() (last = add-new)
};

using State = std::variant<Closed, Picking, OAuthCode, OAuthExchanging,
                           ChatGptWaiting, ApiKeyInput, CustomHostInput,
                           AccountList, Failed>;

[[nodiscard]] inline bool is_open(const State& s) noexcept {
    return !std::holds_alternative<Closed>(s);
}

[[nodiscard]] inline bool is_input_state(const State& s) noexcept {
    // States that consume free-text key input (vs the Picking choice keys).
    return std::holds_alternative<OAuthCode>(s)
        || std::holds_alternative<ApiKeyInput>(s)
        || std::holds_alternative<CustomHostInput>(s);
}

} // namespace agentty::ui::login
