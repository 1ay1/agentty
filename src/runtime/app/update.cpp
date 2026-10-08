// agentty::app::update — pure (Model, Msg) -> (Model, Cmd) reducer.
//
// Top-level orchestrator: a single 10-arm std::visit that dispatches on
// the domain (msg::ComposerMsg / msg::StreamMsg / …) and forwards to
// the matching per-domain reducer in update/<domain>.cpp.
//
// The previous version of this file inlined all 79 leaf arms in one
// overload{} — sizeof(Msg) was pinned by the heaviest leaf no matter
// which path was active, the std::visit instantiated a 79×N dispatch
// table, and any leaf change forced this whole TU to rebuild (~19 s on
// modest hardware). v2 splits the work: leaves are grouped into 10
// domain sub-variants in msg.hpp; each domain has its own visit in its
// own TU, so:
//
//   • this file's std::visit is 10 arms, one tiny dispatch table.
//   • update/<domain>.cpp recompiles only when its own leaves change.
//   • call sites still construct Msg via `Msg{ComposerEnter{}}` /
//     `dispatch(StreamTextDelta{...})` — std::variant's converting
//     constructor walks each domain alternative; only the matching
//     domain accepts a given leaf, so the wrap is unambiguous.

#include "agentty/runtime/app/update.hpp"

#include <utility>

#include <maya/core/overload.hpp>

#include "agentty/runtime/app/update/internal.hpp"

namespace agentty::app {

using maya::overload;

// (Removed) `is_user_input` previously gated the `needs_force_redraw`
// consumer below. That flag is gone — see model.hpp's comment for the
// rationale (the maya-side renderer fix made the post-stream redraw
// unnecessary, and firing it on every first keystroke was actively
// causing the scrollback-duplication symptom it was meant to prevent).

// The pair-returning whole-Msg entry point, for tests.
//
// jaal does NOT call this — it walks the Msg tree and calls the per-domain
// overloads through AgenttyApp, which stamps m.now from jaal's argument.
// This stays because ~40 tests drive the reducer as `update(model, msg)`
// and reading the result as a value is what makes them legible.
//
// `now` is the fold's time, exactly as jaal would hand it. A test that cares
// about time passes it; one that doesn't gets kTestEpoch, a FIXED instant —
// never the wall clock. That is the point: the old entry let every reducer
// read std::chrono itself, so a test's outcome could depend on how long the
// machine took to run it. Now a test is reproducible whether or not it names
// a time, and advancing time is something a test does on purpose.
std::pair<Model, Cmd> update(Model m, Msg msg) {
    return update(std::move(m), std::move(msg), kTestEpoch);
}

std::pair<Model, Cmd> update(Model m, Msg msg,
                             std::chrono::steady_clock::time_point now) {
    m.now = now;   // the fold's time: see Model::now
    // A Model a test built directly never ran init(), so it has no calendar
    // anchor and wall_now() would report 1970. Anchor it to a fixed date the
    // first time it folds — deterministic, so a test that checks updated_at
    // gets the same answer every run, and a real date, so code that sorts or
    // formats it behaves as it does in the app.
    if (m.steady_epoch == std::chrono::steady_clock::time_point{}) {
        m.steady_epoch = kTestEpoch;
        m.wall_epoch   = std::chrono::system_clock::time_point{
            std::chrono::seconds{1'767'225'600}};   // 2026-01-01T00:00:00Z
    }
    // One-shot warmup flag: set by ThreadLoaded, consumed by maya's
    // run loop on the very next render(). Clear on every subsequent
    // reducer step so a later thread load sees a clean false→true
    // edge (maya's loop only fires warmup_render on rising edges).
    // The ThreadLoaded handler in picker.cpp will set it back true
    // for its own swap before this clear-by-next-step path runs.
    const bool is_thread_load = std::visit([](const auto& x) {
        using T = std::decay_t<decltype(x)>;
        if constexpr (std::is_same_v<T, msg::ThreadListMsg>) {
            return std::holds_alternative<::agentty::ThreadLoaded>(x);
        }
        return false;
    }, msg);
    if (!is_thread_load) m.ui.needs_warmup_render = false;

    // Every domain reducer is `Cmd(Model&, DomainMsg)` now, so each arm is
    // a direct call and the model needs no round trip.
    auto cmd = std::visit(overload{
        [&](msg::ComposerMsg cm)     { return detail::composer_update     (m, std::move(cm)); },
        [&](msg::StreamMsg sm)       { return detail::stream_update       (m, std::move(sm)); },
        [&](msg::ToolMsg tm)         { return detail::tool_update         (m, std::move(tm)); },
        [&](msg::ToolOutputMsg tm)   { return detail::tool_output_update  (m, std::move(tm)); },
        [&](msg::ProvidersMsg pm)    { return detail::providers_update    (m, std::move(pm)); },
        [&](msg::ModelsMsg pm)       { return detail::models_update       (m, std::move(pm)); },
        [&](msg::ThreadListMsg tm)   { return detail::thread_list_update  (m, std::move(tm)); },
        [&](msg::PaletteMsg pm)      { return detail::palette_update      (m, std::move(pm)); },
        [&](msg::MentionMsg mm)      { return detail::mention_update      (m, std::move(mm)); },
        [&](msg::SymbolMsg sm)       { return detail::symbol_update       (m, std::move(sm)); },
        [&](msg::CodeBlockMsg cm)    { return detail::codeblock_update    (m, std::move(cm)); },
        [&](msg::CheckpointMsg cm)   { return detail::checkpoint_update   (m, std::move(cm)); },
        [&](msg::RagMsg rm)          { return detail::rag_settings_update (m, std::move(rm)); },
        [&](msg::StatsMsg sm)        { return detail::stats_update        (m, std::move(sm)); },
        [&](msg::SettingsListMsg sm) { return detail::settings_list_update(m, std::move(sm)); },
        [&](msg::ForkMsg fm)         { return detail::fork_update         (m, std::move(fm)); },
        [&](msg::TodoMsg tm)         { return detail::todo_update         (m, std::move(tm)); },
        [&](msg::LoginMsg lm)        { return detail::login_update        (m, std::move(lm)); },
        [&](msg::DiffReviewMsg dm)   { return detail::diff_review_update  (m, std::move(dm)); },
        [&](msg::SmartModeMsg sm)    { return detail::smart_mode_update   (m, std::move(sm)); },
        [&](msg::PluginEditMsg pm)   { return detail::plugin_edit_update  (m, std::move(pm)); },
        [&](msg::AppearanceMsg am)   { return detail::appearance_update   (m, std::move(am)); },
        [&](msg::SandboxMsg sm)      { return detail::sandbox_update      (m, std::move(sm)); },
        [&](msg::MetaMsg mm)         { return detail::meta_update         (m, std::move(mm)); },
    }, msg);

    // Same post-fold step the live seam runs (program.hpp), so a test sees the
    // publish effects the app would emit.
    cmd = publish_derived(m, std::move(cmd));
    return {std::move(m), std::move(cmd)};
}

// ── Model-derived state the world reads ────────────────────────────────────────────────
//
// The subagent router runs on worker threads and reads a registry, so the
// registry has to track four Model fields. This is the ONE place that keeps
// it in step: derive the view the Model implies, and publish only when it
// differs from what was last published.
//
// Pure in the Elm sense: it reads the Model, records what it published there
// (so the comparison is against Model state, not a hidden static), and
// RETURNS the effect. The host performs it (host.hpp handle(PublishSubagent)).
//
// Equality, not a hash, decides "changed": a collision would silently keep a
// stale router running, and equality can't be wrong in that direction — the
// same reasoning as jaal's subs_key (D38).
Cmd publish_derived(Model& m, Cmd c) {
    // The active provider. The Model owns it; code off the loop (the stream
    // worker, ACP, main) reads a published copy in a process global. Same
    // shape as the subagent view below: compare with what was last
    // published, and when it moved, RETURN an effect the host performs.
    // No write from here — this function only describes.
    //
    // Ordering is safe: jaal interprets a fold's Cmd on this thread right
    // after update returns, so the publish lands before any later fold runs;
    // and a Cmd from THIS fold that needs the provider (launch_stream,
    // fetch_models) already captured m.d.selection by value.
    Cmd sel_effect = Cmd::none();
    if (!(m.published_selection == m.d.selection)) {
        m.published_selection = m.d.selection;
        sel_effect = Cmd(PublishSelection{m.d.selection});
    }

    // Compare IN PLACE against the Model, field by field. This runs after
    // every fold, keystrokes included, and the candidate list can be hundreds
    // of models: building a view first would copy that vector on every
    // keystroke just to find out nothing changed. Copies happen only on the
    // rare fold that actually moved one of these fields.
    auto& pub = m.published_subagent;
    const std::string provider = detail::active_provider_id(m);
    Cmd sub_effect = Cmd::none();
    if (!(pub.model == m.d.model_id.value && pub.provider == provider
          && pub.smart == m.d.smart && pub.candidates == m.d.available_models)) {
        pub.model      = m.d.model_id.value;
        pub.provider   = provider;
        pub.smart      = m.d.smart;
        pub.candidates = m.d.available_models;
        sub_effect = Cmd(PublishSubagent{
            .model = pub.model, .provider = pub.provider,
            .smart = pub.smart, .candidates = pub.candidates,
        });
    }

    // Nothing moved — the common case, every keystroke — costs no batch.
    if (sel_effect.is_none() && sub_effect.is_none()) return c;
    // The selection is published before the subagent view, so a worker that
    // reads both sees them describe the same provider.
    return Cmd::batch(std::move(c), std::move(sel_effect), std::move(sub_effect));
}

namespace {

// One-shot warmup flag: set by ThreadLoaded, consumed by maya's host on the
// very next render(). Clear it on every OTHER reducer step so a later thread
// load still produces a clean false->true edge (the host only fires
// warmup_render on a rising edge). The ThreadLoaded handler in picker.cpp
// sets it back true for its own swap, after this runs.
//
// Lives here rather than inline in each overload because it must happen for
// EVERY domain, exactly once per step, before the reducer sees the model.
template <class Domain>
void clear_warmup_unless_thread_load(Model& m, const Domain& d) {
    bool is_thread_load = false;
    if constexpr (std::is_same_v<Domain, msg::ThreadListMsg>)
        is_thread_load = std::holds_alternative<::agentty::ThreadLoaded>(d);
    if (!is_thread_load) m.ui.needs_warmup_render = false;
}

} // namespace

// ── jaal's entry points ───────────────────────────────────────────────────
// One overload per domain. jaal walks the Msg tree and calls the one that
// matches what it landed on; we no longer write the outer visit.
//
// Each is now a direct call: the reducers are all `Cmd(Model&, DomainMsg)`,
// so there is no model to hand back and forth.
//
// Done as a macro because 23 identical bodies written out is 23 chances to
// typo one of them, and a typo here routes a whole domain to the wrong
// reducer — a bug the compiler cannot see, since every reducer has the same
// shape. The macro is undefined immediately after.
//
// `clear_warmup_unless_thread_load` is not incidental: the old dispatcher ran
// it before EVERY step, and dropping it would have left the warmup flag stuck
// on after the first thread load, so maya re-warmed the render cache on every
// frame. It fires here for the same reason it did there — see its comment.
#define AGENTTY_DOMAIN_UPDATE(DomainMsg, reducer)                       \
    Cmd update(Model& m, msg::DomainMsg d) {                            \
        clear_warmup_unless_thread_load(m, d);                          \
        return detail::reducer(m, std::move(d));                        \
    }

AGENTTY_DOMAIN_UPDATE(ComposerMsg,     composer_update)
AGENTTY_DOMAIN_UPDATE(StreamMsg,       stream_update)
AGENTTY_DOMAIN_UPDATE(ToolMsg,         tool_update)
AGENTTY_DOMAIN_UPDATE(ToolOutputMsg,   tool_output_update)
AGENTTY_DOMAIN_UPDATE(ProvidersMsg,    providers_update)
AGENTTY_DOMAIN_UPDATE(ModelsMsg,       models_update)
AGENTTY_DOMAIN_UPDATE(ThreadListMsg,   thread_list_update)
AGENTTY_DOMAIN_UPDATE(PaletteMsg,      palette_update)
AGENTTY_DOMAIN_UPDATE(MentionMsg,      mention_update)
AGENTTY_DOMAIN_UPDATE(SymbolMsg,       symbol_update)
AGENTTY_DOMAIN_UPDATE(CodeBlockMsg,    codeblock_update)
AGENTTY_DOMAIN_UPDATE(CheckpointMsg,   checkpoint_update)
AGENTTY_DOMAIN_UPDATE(RagMsg,          rag_settings_update)
AGENTTY_DOMAIN_UPDATE(StatsMsg,        stats_update)
AGENTTY_DOMAIN_UPDATE(SettingsListMsg, settings_list_update)
AGENTTY_DOMAIN_UPDATE(ForkMsg,         fork_update)
AGENTTY_DOMAIN_UPDATE(TodoMsg,         todo_update)
AGENTTY_DOMAIN_UPDATE(LoginMsg,        login_update)
AGENTTY_DOMAIN_UPDATE(DiffReviewMsg,   diff_review_update)
AGENTTY_DOMAIN_UPDATE(SmartModeMsg,    smart_mode_update)
AGENTTY_DOMAIN_UPDATE(PluginEditMsg,   plugin_edit_update)
AGENTTY_DOMAIN_UPDATE(AppearanceMsg,   appearance_update)
AGENTTY_DOMAIN_UPDATE(SandboxMsg,      sandbox_update)
AGENTTY_DOMAIN_UPDATE(MetaMsg,         meta_update)

#undef AGENTTY_DOMAIN_UPDATE

} // namespace agentty::app
