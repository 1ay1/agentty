// turn_provenance_test — an assistant turn is labelled by the model that
// ACTUALLY served it, not by whatever the picker happens to hold now.
//
// The bug: under Smart Mode the turn is dispatched on the resolved role model
// (`strategic_profile.model` in launch_stream), which is routinely NOT the
// model in the picker. Both the status-bar badge and the turn header read
// `Model::d.model_id` — the SELECTION — so agentty displayed "Mistral Large"
// while every byte of the reply came from GLM. The debug log (which reports
// the wire) and the UI (which reported the selection) disagreed, and the UI
// was wrong.
//
// It also lied retroactively: because the header was computed from live state
// at render time, switching models relabelled every turn already sitting in
// the transcript. A turn's provenance is a property OF THE TURN, so it now
// lives on the turn (Message::served_model / served_role), is stamped once at
// StreamStarted, survives persistence, and is mixed into the render key.

#include <string>

#include "agtest.hpp"

#include "agentty/domain/conversation.hpp"
#include "agentty/runtime/model.hpp"
#include "agentty/runtime/app/deps.hpp"
#include "agentty/runtime/app/update.hpp"
#include "agentty/provider/selection.hpp"
#include "agentty/store/store.hpp"

using namespace agentty;
namespace store = agentty::store;

namespace {

store::Settings g_settings;

void install_stub_deps() {
    app::install_deps(app::Deps{
        .stream         = [](auto, auto) {},
        .save_thread    = [](const Thread&) {},
        .delete_thread  = [](const auto&) {},
        .load_threads   = [] { return std::vector<Thread>{}; },
        .load_thread    = [](const ThreadId&) { return std::optional<Thread>{}; },
        .load_settings  = [] { return g_settings; },
        .save_settings  = [](const store::Settings& s) { g_settings = s; },
        .new_thread_id  = [] { return ThreadId{"t-provenance"}; },
        .title_from     = [](std::string_view t) { return std::string{t}; },
        .auth           = {},
    });
}

// A model mid-turn: one user message, one assistant placeholder, the wire
// about to open. `routed` is what launch_stream resolved for this turn
// (empty = Smart Mode off, so the selection serves it).
Model mid_turn(const std::string& selected, const std::string& routed,
               const std::string& role) {
    Model m;
    m.d.model_id = ModelId{selected};
    Message user;
    user.role = Role::User;
    user.text = "hello";
    m.d.current.messages.push_back(std::move(user));
    Message asst;
    asst.role = Role::Assistant;
    m.d.current.messages.push_back(std::move(asst));
    m.s.smart_turn_model = routed;
    m.s.smart_turn_role  = role;
    return m;
}

} // namespace

TEST_CASE("turn provenance: the header names the model that served the turn") {
    install_stub_deps();
    {
        provider::Selection sel;
        sel.kind = provider::Kind::Anthropic;
        provider::select(sel);
    }

    // ── StreamStarted stamps the routed model onto the assistant turn ──
    {
        Model m = mid_turn("mistral-large-3", "glm-5.3", "strategic");
        auto [m1, _] = app::update(std::move(m), Msg{StreamStarted{}});
        const auto& asst = m1.d.current.messages.back();
        CHECK(asst.served_model == "glm-5.3",
              "the turn records the model that actually served it");
        CHECK(asst.served_role == "strategic",
              "the turn records the role it was routed as");
        CHECK(m1.d.model_id.value == "mistral-large-3",
              "stamping provenance must not disturb the user's selection");
    }

    // ── Smart Mode off: no stamp, the header falls back to the selection ──
    {
        Model m = mid_turn("mistral-large-3", "", "");
        auto [m1, _] = app::update(std::move(m), Msg{StreamStarted{}});
        const auto& asst = m1.d.current.messages.back();
        CHECK(asst.served_model.empty(),
              "no Smart Mode routing ⇒ no stamp (the selection served it)");
        CHECK(asst.served_role.empty(), "and no role tag");
    }

    // ── Switching models does NOT relabel turns already in the transcript ──
    // This is the retroactive half of the bug: the header used to be derived
    // from live state, so a settled GLM turn became a "Mistral" turn the
    // instant you switched.
    {
        Model m = mid_turn("mistral-large-3", "glm-5.3", "strategic");
        auto [m1, _] = app::update(std::move(m), Msg{StreamStarted{}});
        m1.d.model_id = ModelId{"claude-opus-4-5"};   // user switches after
        CHECK(m1.d.current.messages.back().served_model == "glm-5.3",
              "a settled turn keeps its author across a model switch");
    }

    // ── Provenance changes the render key (else the cached Element sticks) ──
    {
        Message a;
        a.role = Role::Assistant;
        a.text = "same text";
        Message b = a;
        b.served_model = "glm-5.3";
        b.served_role  = "strategic";
        CHECK(a.compute_render_key() != b.compute_render_key(),
              "served_model/role must invalidate the per-message render cache");
    }
}
