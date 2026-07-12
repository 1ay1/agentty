#include "agentty/runtime/app/program.hpp"
#include "agentty/runtime/app/cmd_factory.hpp"
#include "agentty/runtime/login.hpp"
#include "agentty/runtime/view/helpers.hpp"
#include "agentty/auth/auth.hpp"
#include "agentty/provider/selection.hpp"

#include <vector>

namespace agentty::app {

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
    // Seed the composer idle-blink clock at launch so the 15 s
    // blink-stop countdown starts now, not on the first keystroke. A
    // freshly-opened, never-touched agentty is exactly the idle-CPU case
    // (a forever-blinking painted cursor keeps a GPU terminal compositing),
    // so it must time out even if the user never types.
    m.ui.composer.last_edit_ms = maya::anim::default_clock().now_ms();
    // Thread history is the single largest startup cost: a real-world
    // history of hundreds of multi-MB thread JSONs serializes into
    // seconds of synchronous parse work before the first frame can
    // render. Defer to a background task; ThreadsLoaded fills the list
    // when it lands. The thread picker reads `m.s.threads_loading` to
    // show a "loading…" hint until then.
    m.s.threads_loading  = true;
    m.d.available_models = seed_models();

    auto settings = deps().load_settings();
    if (!settings.model_id.empty()) {
        // Guard against a cross-provider model id collision. A persisted
        // model id belongs to whatever provider was active when it was
        // saved; relaunching on a DIFFERENT provider (or with no
        // --provider, falling back to Anthropic) must not carry that id
        // along — e.g. a leftover "qwen2.5-coder:7b" sent to Anthropic
        // 404s on the first prompt. For Anthropic, only honour the saved
        // id if it's a known Claude id; otherwise drop it and let the
        // seed default (model_id's built-in) stand. The OpenAI side
        // self-corrects via the eager fetch_models() round trip below,
        // so it keeps the saved id and lets ModelsLoaded validate it.
        const bool anthropic_active =
            provider::active().kind == provider::Kind::Anthropic;
        bool honour = true;
        if (anthropic_active) {
            // Honour the saved id if it's a known seeded model OR any
            // `claude-*` id. The seed list is a small hardcoded snapshot
            // (opus/sonnet/haiku 4.5); a NEWER Claude the user selected
            // (e.g. claude-opus-4-8) is a perfectly valid Anthropic id but
            // isn't seeded, and rejecting it here is exactly why the last
            // picked model was forgotten every launch — we fell back to the
            // built-in default. Any `claude-` prefix is an Anthropic id and
            // is safe to send; only a foreign id (e.g. a leftover Ollama
            // "qwen2.5-coder:7b") must be dropped so it doesn't 404.
            honour = settings.model_id.value.starts_with("claude-");
            if (!honour) {
                for (const auto& mi : m.d.available_models)
                    if (mi.id == settings.model_id) { honour = true; break; }
            }
        }
        if (honour) {
            m.d.model_id = settings.model_id;
            // Ensure the honoured id is in the picker list (a newer Claude
            // won't be in the seed snapshot) so the model picker shows the
            // active model selected instead of nothing.
            bool present = false;
            for (const auto& mi : m.d.available_models)
                if (mi.id == m.d.model_id) { present = true; break; }
            if (!present)
                m.d.available_models.insert(
                    m.d.available_models.begin(),
                    ModelInfo{m.d.model_id, m.d.model_id.value,
                              "anthropic", 200000, false});
        }
    }
    // Set the per-model context window now (before any stream runs) so
    // the ctx % bar uses the right denominator from frame 1, not after
    // the user's first message lands.
    m.s.context_max = ui::context_max_for_model(m.d.model_id.value);
    m.d.profile = settings.profile;
    m.d.effort  = effort_from_wire(settings.effort);
    m.d.review_enabled = settings.review_enabled;
    // Rehydrate persisted "always allow" tool grants (Zed's always_allow
    // rules). PermissionApproveAlways appends to this list; loading it here
    // means a grant given last week still suppresses the prompt today.
    for (const auto& g : settings.always_allow_tools)
        m.d.session_grants.insert(g);
    for (auto& mi : m.d.available_models)
        for (const auto& fav : settings.favorite_models)
            if (mi.id == fav) mi.favorite = true;

    m.d.current.id  = deps().new_thread_id();
    m.s.status = "ready";

    // No credentials installed yet → main() invoked install() with an
    // empty header. Open the login modal so the user can authenticate
    // without leaving the TUI; subscribe.cpp routes all input there
    // until they finish (or Esc out, leaving agentty unauth'd — they'll
    // hit a stream error on first send and can /login from the palette).
    //
    // Gate on the active provider being Anthropic: the modal is
    // Anthropic-specific (OAuth / sk-ant key). An OpenAI-family backend
    // authenticates via an env var resolved at startup, so an empty
    // header there means "no key in env" — popping the Anthropic OAuth
    // modal would be nonsensical. Those users get a stream error naming
    // the missing key on first send instead.
    if (auth::is_empty(deps().auth)
        && provider::active().kind == provider::Kind::Anthropic)
        m.ui.login = ui::login::Picking{};

    std::vector<maya::Cmd<Msg>> cmds;
    cmds.push_back(cmd::load_threads_async());

    // OpenAI-family backends (Ollama, llama.cpp, groq, …) have no fixed
    // built-in model list — seed_models() only knows Claude ids. A saved
    // or --provider-restored model id may not be served by the active
    // endpoint (e.g. "qwen2.5-coder:7b" persisted but the daemon never
    // pulled it, or the id belongs to a different backend), which 404s
    // on the very first prompt. Fetch /v1/models eagerly at startup so
    // the ModelsLoaded reducer can swap to a real, served model id
    // BEFORE the user sends anything — the same validation the model
    // picker does, just not gated on the user opening it. Anthropic has
    // a trustworthy built-in seed list, so it skips this round trip.
    if (provider::active().kind == provider::Kind::OpenAI) {
        m.s.models_loading = true;
        cmds.push_back(cmd::fetch_models());
    }

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

} // namespace agentty::app
