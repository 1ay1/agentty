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

// Call a reducer of either shape, leaving the new model in `m`.
//
// The reducers are mid-conversion to jaal's `Cmd(Model&, DomainMsg)`. The
// jaal form already writes through the reference and hands back only the
// effect; the legacy `Step(Model, DomainMsg)` returns (model, effect), so
// the model has to be moved back. That move IS the adapter, and it
// disappears with the last unconverted domain.
//
// Having it in one place is what lets a domain convert on its own instead
// of all 23 changing in a single commit.
namespace {

template <class R, class D>
[[nodiscard]] Cmd call_reducer(R reducer, Model& m, D d) {
    if constexpr (std::is_invocable_r_v<Cmd, R, Model&, D>) {
        return reducer(m, std::move(d));
    } else {
        auto [next, cmd] = reducer(std::move(m), std::move(d));
        m = std::move(next);
        return std::move(cmd);
    }
}

} // namespace

// The pair-returning whole-Msg entry point.
//
// jaal does NOT call this — it walks the Msg tree and calls the per-domain
// `update(Model&, DomainMsg)` overloads below. This stays because ~40 tests
// drive the reducer as `update(model, msg)` and reading the result as a
// value is what makes them legible. It delegates, so there is still one
// implementation per domain.
std::pair<Model, Cmd> update(Model m, Msg msg) {
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

    // Each arm hands the model to its domain reducer through call_reducer,
    // which copes with both the jaal shape and the legacy one — so this
    // function does not need to know which domains have been converted.
    auto cmd = std::visit(overload{
        [&](msg::ComposerMsg cm)     { return call_reducer(detail::composer_update,      m, std::move(cm)); },
        [&](msg::StreamMsg sm)       { return call_reducer(detail::stream_update,        m, std::move(sm)); },
        [&](msg::ToolMsg tm)         { return call_reducer(detail::tool_update,          m, std::move(tm)); },
        [&](msg::ToolOutputMsg tm)   { return call_reducer(detail::tool_output_update,   m, std::move(tm)); },
        [&](msg::ProvidersMsg pm)    { return call_reducer(detail::providers_update,     m, std::move(pm)); },
        [&](msg::ModelsMsg pm)       { return call_reducer(detail::models_update,        m, std::move(pm)); },
        [&](msg::ThreadListMsg tm)   { return call_reducer(detail::thread_list_update,   m, std::move(tm)); },
        [&](msg::PaletteMsg pm)      { return call_reducer(detail::palette_update,       m, std::move(pm)); },
        [&](msg::MentionMsg mm)      { return call_reducer(detail::mention_update,       m, std::move(mm)); },
        [&](msg::SymbolMsg sm)       { return call_reducer(detail::symbol_update,        m, std::move(sm)); },
        [&](msg::CodeBlockMsg cm)    { return call_reducer(detail::codeblock_update,     m, std::move(cm)); },
        [&](msg::CheckpointMsg cm)   { return call_reducer(detail::checkpoint_update,    m, std::move(cm)); },
        [&](msg::RagMsg rm)          { return call_reducer(detail::rag_settings_update,  m, std::move(rm)); },
        [&](msg::StatsMsg sm)        { return call_reducer(detail::stats_update,         m, std::move(sm)); },
        [&](msg::SettingsListMsg sm) { return call_reducer(detail::settings_list_update, m, std::move(sm)); },
        [&](msg::ForkMsg fm)         { return call_reducer(detail::fork_update,          m, std::move(fm)); },
        [&](msg::TodoMsg tm)         { return call_reducer(detail::todo_update,          m, std::move(tm)); },
        [&](msg::LoginMsg lm)        { return call_reducer(detail::login_update,         m, std::move(lm)); },
        [&](msg::DiffReviewMsg dm)   { return call_reducer(detail::diff_review_update,   m, std::move(dm)); },
        [&](msg::SmartModeMsg sm)    { return call_reducer(detail::smart_mode_update,    m, std::move(sm)); },
        [&](msg::PluginEditMsg pm)   { return call_reducer(detail::plugin_edit_update,   m, std::move(pm)); },
        [&](msg::AppearanceMsg am)   { return call_reducer(detail::appearance_update,    m, std::move(am)); },
        [&](msg::MetaMsg mm)         { return call_reducer(detail::meta_update,          m, std::move(mm)); },
    }, msg);

    return {std::move(m), std::move(cmd)};
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
// Each hands the reducer the model and returns the effect. The reducers are
// mid-conversion to jaal's `Cmd(Model&, DomainMsg)`, so `call_reducer` below
// accepts BOTH that and the old `Step(Model, DomainMsg)` — which is what
// lets a domain move on its own instead of all 23 changing in one commit.
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
        return call_reducer(detail::reducer, m, std::move(d));          \
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
AGENTTY_DOMAIN_UPDATE(MetaMsg,         meta_update)

#undef AGENTTY_DOMAIN_UPDATE

} // namespace agentty::app
