#pragma once
// agentty::ui_prefs::live — the appearance prefs, readable from the view.
//
// ── Why a seam and not a parameter ───────────────────────────────────────
//
// The prefs live on the Model, and the view is a pure function of it — so
// in principle every consumer should take them as an argument. In practice
// the consumers are places like `panel_viewport_h()`, a free function called
// from twenty panel builders, and the StreamingMarkdown setup buried in
// turn.cpp; threading `const Prefs&` to each would mean changing dozens of
// signatures that have nothing else to do with appearance, and would put a
// prefs parameter on functions whose callers don't have a Model either.
//
// So the prefs are PUBLISHED once per frame, exactly as maya publishes its
// theme slot (app_set_theme) and for exactly the same reason: the swap has
// to reach code that cannot be handed a Model without rewriting the world
// around it. `view()` calls publish() before building anything; everything
// downstream reads current().
//
// ── What this is NOT ─────────────────────────────────────────────────────
//
// Not a place to stash mutable UI state. It holds a COPY of one field of
// the Model, refreshed from the Model every frame, and nothing writes to it
// except publish(). A reducer must never call publish() — the Model is the
// truth and this is a projection of it, so writing here instead would put
// the truth in two places and let them disagree.
//
// ── Thread safety ────────────────────────────────────────────────────────
//
// Published on the render thread, read on the render thread. The lock is
// for the background workers that build tool previews off-frame: they get a
// consistent snapshot rather than a torn read, which is all they need —
// they do not care WHICH frame's prefs they see, only that it is one of
// them.

#include <mutex>
#include <shared_mutex>

#include "agentty/domain/ui_prefs.hpp"

namespace agentty::ui_prefs {

namespace detail {
// Prefs holds a std::string (the theme name), so it is not trivially
// copyable and cannot live in a std::atomic. A mutex + a value is the
// honest implementation: the lock is uncontended in practice (published
// once per frame on the render thread, read on the same thread) and a
// shared_mutex's read side is a relaxed atomic increment, which is the
// same order of cost the atomic would have been.
inline std::shared_mutex& slot_mutex() noexcept {
    static std::shared_mutex mu;
    return mu;
}
inline Prefs& slot() noexcept {
    static Prefs s{};
    return s;
}
}  // namespace detail

// Make `p` the prefs every consumer sees from now on. Called once per frame
// by view(), before any Element is built.
inline void publish(const Prefs& p) {
    std::unique_lock lk{detail::slot_mutex()};
    detail::slot() = p;
}

// The prefs in force. Defaults until the first publish(), so a consumer
// that runs before the first frame (a test, a background prewarm) gets the
// documented defaults rather than garbage.
[[nodiscard]] inline Prefs current() {
    std::shared_lock lk{detail::slot_mutex()};
    return detail::slot();
}

// ── Convenience readers ──────────────────────────────────────────────────
//
// Named for the QUESTION each consumer asks, not for the field it reads.
// A consumer that says `animations_on()` keeps working if the motion model
// grows a fourth level; one that says `current().motion == Motion::Full`
// has to be found and fixed.

// Whether anything may move: the streaming reveal, spinners, transitions.
[[nodiscard]] inline bool animations_on() {
    return current().motion != Motion::Off;
}

// Whether the DECORATIVE layer of the reveal may run (scramble, gradient,
// blinking caret) as opposed to the progressive clip. Reduced keeps text
// walking in — which is information about progress — and drops the glyph
// churn, which is not.
[[nodiscard]] inline bool reveal_decoration_on() {
    return current().motion == Motion::Full;
}

// The panel height ceiling, in rows. Still a MAXIMUM — the caller clamps to
// what the terminal actually has.
[[nodiscard]] inline int panel_rows() {
    return viewport_rows(current().density);
}

}  // namespace agentty::ui_prefs
