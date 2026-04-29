// Composer-submission, thread-virtualization, and settings-persistence
// helpers for the update reducer. submit_message is the entry point for
// ComposerEnter / ComposerSubmit and is also called from finalize_turn when
// flushing the composer's queued-message buffer, which is why it lives in a
// shared internal header rather than an anonymous namespace.

#include "moha/runtime/app/update/internal.hpp"

#include <algorithm>
#include <chrono>
#include <utility>

#include "moha/runtime/app/cmd_factory.hpp"
#include "moha/runtime/app/deps.hpp"
#include "moha/store/store.hpp"

namespace moha::app::detail {

namespace {

// Row-height estimate for a message — used only by the virtualization slice
// to tell maya how many rows of prev-frame to commit to scrollback.
// Conservative; an imperfect estimate costs at most one visible refresh of
// the window on the slice frame.
int estimate_message_rows(const Message& msg) {
    constexpr int kEstWidth = 100;
    int rows = 3;
    if (!msg.text.empty()) {
        const int w = std::max(1, kEstWidth - 4);
        rows += static_cast<int>(msg.text.size()) / w + 1;
        int nl = 0;
        for (char c : msg.text) if (c == '\n') ++nl;
        rows += nl;
    }
    for (const auto& tc : msg.tool_calls) {
        rows += 4;
        const auto& out = tc.output();
        if (!out.empty()) {
            int nl = 0;
            for (char c : out) if (c == '\n') ++nl;
            rows += std::min(nl, 10);
        }
    }
    return rows;
}

} // namespace

maya::Cmd<Msg> maybe_virtualize(Model& m) {
    using maya::Cmd;
    const int total = static_cast<int>(m.d.current.messages.size());
    const int visible = total - m.ui.thread_view_start;
    // Only slice in discrete chunks — a one-per-turn slice would refresh
    // the visible area every turn, whereas chunking it causes one refresh
    // every kSliceChunk turns.
    if (visible <= kViewWindow + kSliceChunk) return Cmd<Msg>::none();

    int committed_rows = 0;
    int committed_turns = 0;
    for (int i = m.ui.thread_view_start; i < m.ui.thread_view_start + kSliceChunk; ++i) {
        committed_rows += estimate_message_rows(m.d.current.messages[i]);
        if (m.d.current.messages[i].role == Role::Assistant) ++committed_turns;
    }
    m.ui.thread_view_start      += kSliceChunk;
    m.ui.thread_view_start_turn += committed_turns;
    return Cmd<Msg>::commit_scrollback(committed_rows);
}

Step submit_message(Model m) {
    using maya::Cmd;
    if (m.ui.composer.text.empty()) return done(std::move(m));

    if (m.s.is_streaming() || m.s.is_executing_tool()) {
        m.ui.composer.queued.push_back(std::exchange(m.ui.composer.text, {}));
        m.ui.composer.cursor = 0;
        return done(std::move(m));
    }

    Message user;
    user.role = Role::User;
    user.text = std::exchange(m.ui.composer.text, {});
    m.ui.composer.cursor = 0;
    if (m.d.current.title.empty())
        m.d.current.title = deps().title_from(user.text);
    m.d.current.messages.push_back(std::move(user));

    Message placeholder;
    placeholder.role = Role::Assistant;
    m.d.current.messages.push_back(std::move(placeholder));

    m.d.current.updated_at = std::chrono::system_clock::now();

    // Idle → Streaming. The fresh phase::Active replaces the prior
    // turn's context wholesale (Idle had none): zero retry counters,
    // fresh started/last_event_at stamps, default RetryState. Mirrors
    // the StreamStarted handler's reset so the post-submit render is
    // layout-identical to the post-StreamStarted render that lands
    // milliseconds later — without this, leftover status toast from
    // the prior turn (retry countdown / "Stream complete" / error
    // banner) would change status_bar height by one row when
    // StreamStarted fires, producing a visible "new turn appears at
    // viewport bottom and then realigns" two-frame flicker.
    auto now = std::chrono::steady_clock::now();
    phase::Active ctx;
    ctx.started       = now;
    ctx.last_event_at = now;
    m.s.phase         = phase::Streaming{std::move(ctx)};
    m.s.status.clear();
    m.s.status_until  = {};

    auto virt = maybe_virtualize(m);
    auto launch = cmd::launch_stream(m);
    auto cmd = virt.is_none()
        ? std::move(launch)
        : Cmd<Msg>::batch(std::vector<Cmd<Msg>>{std::move(virt), std::move(launch)});
    return {std::move(m), std::move(cmd)};
}

void persist_settings(const Model& m) {
    store::Settings s;
    s.model_id = m.d.model_id;
    s.profile  = m.d.profile;
    for (const auto& mi : m.d.available_models)
        if (mi.favorite) s.favorite_models.push_back(mi.id);
    deps().save_settings(s);
}

maya::Cmd<Msg> set_status_toast(Model& m, std::string text,
                                std::chrono::seconds ttl) {
    using maya::Cmd;
    m.s.status = std::move(text);
    auto now = std::chrono::steady_clock::now();
    m.s.status_until = now + ttl;
    auto stamp = m.s.status_until;
    return Cmd<Msg>::after(
        std::chrono::duration_cast<std::chrono::milliseconds>(ttl)
            + std::chrono::milliseconds{50},
        Msg{ClearStatus{stamp}});
}

} // namespace moha::app::detail
