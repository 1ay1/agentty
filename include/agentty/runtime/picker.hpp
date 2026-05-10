#pragma once
// agentty::ui::pick — picker state machines as sum types.
//
// Replaces the canonical anti-pattern this codebase used to have:
//
//     struct ModelPickerState { bool open = false; int index = 0; };
//
// where `index` is meaningless when `open == false`. A reader had to
// remember which fields were valid in which combination; a writer had to
// remember to reset `index` (or not) when toggling `open`. No type
// system help.
//
// Now: each picker is a closed `std::variant<Closed, Open{cursor…}>`.
// The cursor data is owned by the `Open` alternative — it cannot exist
// when the picker is closed. Adding a new picker shape (e.g. multi-
// select) means a new variant arm + a new `pick::opened` overload; the
// compiler tells you everywhere that needs to change.
//
// Three shapes cover today's pickers:
//   OneAxis  — model list, thread list (single index cursor)
//   TwoAxis  — diff review (file × hunk cursor)
//   Modal    — todo overlay (no cursor; just gates rendering)

#include <variant>

namespace agentty::ui::pick {

struct Closed {};

// One-axis picker (model list, thread list).
struct OpenAt { int index = 0; };
using OneAxis = std::variant<Closed, OpenAt>;

// Two-axis picker (diff review: file × hunk).
struct OpenAtCell { int file_index = 0; int hunk_index = 0; };
using TwoAxis = std::variant<Closed, OpenAtCell>;

// Modal-style picker that toggles open/close without a cursor (e.g.
// the todo overlay — items live alongside on the owning struct).
struct OpenModal {};
using Modal = std::variant<Closed, OpenModal>;

// ── Helpers ───────────────────────────────────────────────────────────
// Generic "is this variant in any non-Closed state?" — works for all
// three shapes so call sites read uniformly: `if (pick::is_open(x))`.
template <class P>
[[nodiscard]] inline bool is_open(const P& p) noexcept {
    return !std::holds_alternative<Closed>(p);
}

// Cursor accessors. Return nullptr when the picker is closed; callers
// use `if (auto* c = pick::opened(p)) …` to combine the open-check
// with cursor extraction in one expression — the moral equivalent of
// `if let Open(c) = p` in Rust.
[[nodiscard]] inline OpenAt*       opened(OneAxis& p)       noexcept { return std::get_if<OpenAt>(&p); }
[[nodiscard]] inline const OpenAt* opened(const OneAxis& p) noexcept { return std::get_if<OpenAt>(&p); }
[[nodiscard]] inline OpenAtCell*       opened(TwoAxis& p)       noexcept { return std::get_if<OpenAtCell>(&p); }
[[nodiscard]] inline const OpenAtCell* opened(const TwoAxis& p) noexcept { return std::get_if<OpenAtCell>(&p); }

// Convenience: index when open, -1 otherwise. Use only at read sites
// that genuinely don't care about the closed-vs-open distinction
// (e.g. comparing to a list size). Prefer `opened()` when possible.
[[nodiscard]] inline int index_or(const OneAxis& p, int fallback = -1) noexcept {
    if (auto* o = std::get_if<OpenAt>(&p)) return o->index;
    return fallback;
}

} // namespace agentty::ui::pick
