#pragma once
// #symbol picker — opens above the composer when the user types `#`
// at a word boundary. Same shape as the `@` picker, over workspace
// symbols instead of file paths. Symbol = (name, path, line) — the chip
// we attach on select carries all three so submit-time expansion can
// splice an excerpt of the file around the declaration.
//
// The state is a FilteredPicker (panel/filtered_picker.hpp): query, cursor,
// snapshot, filter memo and cold-open refill are all inherited from that one
// primitive. This header declares only what is specific to `#`.
//
// The workspace scanner that produces SymbolEntry (and the filter helper)
// live in `workspace/symbols.hpp`; this header is UI-state-only.

#include <string>
#include <tuple>
#include <variant>
#include <vector>

#include "agentty/runtime/panel/filtered_picker.hpp"
#include "agentty/runtime/visual.hpp"
#include "agentty/workspace/symbols.hpp"  // SymbolEntry

namespace agentty {

namespace symbol {

struct Closed {};

[[nodiscard]] inline ui::SnapshotSource<SymbolEntry> symbol_source() {
    return {
        .ready = [] { return symbols_ready(); },
        .fetch = [] { return list_workspace_symbols(); },
    };
}

[[nodiscard]] inline ui::FilterFn<SymbolEntry> symbol_filter() {
    return [](const std::vector<SymbolEntry>& entries, std::string_view q) {
        return filter_symbols(entries, q);
    };
}

struct Open {
    Open() : picker(symbol_source(), symbol_filter()) {}

    ui::FilteredPicker<SymbolEntry> picker;
};

// See mention.hpp: the picker owns every visible axis, and Open's
// user-provided constructor makes it a non-aggregate, so the parts list is
// explicit and the trust claim is stated next to the type it covers.
inline auto visual_parts(const Open& p) {
    return std::make_tuple(visual::ref(p.picker));
}

} // namespace symbol

namespace visual {
template <> inline constexpr bool trusted_parts<symbol::Open> = true;
} // namespace visual

using SymbolState = std::variant<symbol::Closed, symbol::Open>;

[[nodiscard]] inline bool symbol_palette_is_open(const SymbolState& s) noexcept {
    return std::holds_alternative<symbol::Open>(s);
}
[[nodiscard]] inline       symbol::Open* symbol_palette_opened(SymbolState& s)       noexcept { return std::get_if<symbol::Open>(&s); }
[[nodiscard]] inline const symbol::Open* symbol_palette_opened(const SymbolState& s) noexcept { return std::get_if<symbol::Open>(&s); }

} // namespace agentty
