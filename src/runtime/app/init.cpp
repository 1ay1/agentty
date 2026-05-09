#include "moha/runtime/app/program.hpp"
#include "moha/runtime/app/cmd_factory.hpp"
#include "moha/runtime/login.hpp"
#include "moha/runtime/view/helpers.hpp"
#include "moha/auth/auth.hpp"

#include <vector>

namespace moha::app {

namespace {
std::vector<ModelInfo> seed_models() {
    return {
        {ModelId{"claude-opus-4-5"},   "Claude Opus 4.5",   "anthropic", 200000, true},
        {ModelId{"claude-sonnet-4-5"}, "Claude Sonnet 4.5", "anthropic", 200000, true},
        {ModelId{"claude-haiku-4-5"},  "Claude Haiku 4.5",  "anthropic", 200000, false},
    };
}
} // namespace

std::pair<Model, maya::Cmd<Msg>> init() {
    Model m;
    // Thread history is the single largest startup cost: a real-world
    // history of hundreds of multi-MB thread JSONs serializes into
    // seconds of synchronous parse work before the first frame can
    // render. Defer to a background task; ThreadsLoaded fills the list
    // when it lands. The thread picker reads `m.s.threads_loading` to
    // show a "loading…" hint until then.
    m.s.threads_loading  = true;
    m.d.available_models = seed_models();

    auto settings = deps().load_settings();
    if (!settings.model_id.empty()) m.d.model_id = settings.model_id;
    // Set the per-model context window now (before any stream runs) so
    // the ctx % bar uses the right denominator from frame 1, not after
    // the user's first message lands.
    m.s.context_max = ui::context_max_for_model(m.d.model_id.value);
    m.d.profile = settings.profile;
    for (auto& mi : m.d.available_models)
        for (const auto& fav : settings.favorite_models)
            if (mi.id == fav) mi.favorite = true;

    m.d.current.id  = deps().new_thread_id();
    m.s.status = "ready";

    // No credentials installed yet → main() invoked install() with an
    // empty header. Open the login modal so the user can authenticate
    // without leaving the TUI; subscribe.cpp routes all input there
    // until they finish (or Esc out, leaving moha unauth'd — they'll
    // hit a stream error on first send and can /login from the palette).
    if (deps().auth_header.empty())
        m.ui.login = ui::login::Picking{};

    std::vector<maya::Cmd<Msg>> cmds;
    cmds.push_back(cmd::load_threads_async());

    // Background OAuth refresh handoff. `auth::resolve()` parked a
    // refresh token here when it found expired-but-refreshable creds
    // on disk; pick it up and dispatch the network round trip on a
    // worker so the TUI is interactive immediately. Sticky toast (no
    // expiry) — TokenRefreshed clears it on completion. The
    // oauth_refresh_in_flight flag gates submit_message so the user
    // can't fire a stream with the stale auth_header still in Deps.
    if (auto refresh = auth::take_pending_refresh()) {
        m.s.oauth_refresh_in_flight = true;
        m.s.status = "refreshing OAuth token…";
        m.s.status_until = {};
        cmds.push_back(cmd::refresh_oauth(std::move(*refresh)));
    }

    return {std::move(m), maya::Cmd<Msg>::batch(std::move(cmds))};
}

} // namespace moha::app
