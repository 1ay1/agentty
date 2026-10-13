#pragma once
// @file mention picker — opens above the composer when the user types
// `@` on an empty buffer.
//
// The state is a FilteredPicker over workspace file paths: query, cursor,
// snapshot, filter memo and cold-open refill all come from that one
// primitive (see panel/filtered_picker.hpp) rather than being hand-rolled
// here. This header declares only what is specific to `@`: which snapshot
// feeds it and which filter ranks it.
//
// What used to live here — a `cached_matches / cached_query / cached_valid`
// trio plus a free `mention_filtered()` accessor — was a byte-level clone of
// symbol.hpp's, and the pair of them had drifted apart from the palette,
// which never got the memo at all. The refill was worse: it lived in the
// reducer's "printable typed" arm, so opening `@` cold and pressing ↓ or
// Backspace left the picker on "indexing…" permanently.

#include <string>
#include <tuple>
#include <variant>
#include <vector>

#include "agentty/runtime/panel/filtered_picker.hpp"
#include "agentty/runtime/visual.hpp"
#include "agentty/workspace/files.hpp"

namespace agentty {

namespace mention {

struct Closed {};

// The `@` picker's source + ranking, in one place. Building the source from
// the free functions here (rather than at each construction site) is what
// keeps "which snapshot does `@` read" a single fact.
[[nodiscard]] inline ui::SnapshotSource<std::string> file_source() {
    return {
        .ready = [] { return files_ready(); },
        .fetch = [] { return list_workspace_files(); },
    };
}

[[nodiscard]] inline ui::FilterFn<std::string> file_filter() {
    return [](const std::vector<std::string>& files, std::string_view q) {
        return filter_files(files, q);
    };
}

struct Open {
    Open() : picker(file_source(), file_filter()) {}

    ui::FilteredPicker<std::string> picker;
};

// The picker owns every visible axis (query + cursor), so the panel's parts
// list is just the picker's. Declared here rather than in the shared
// visual_parts.hpp because Open now has a user-provided constructor — the
// brace-arity probe reads 0, so the completeness proof needs the explicit
// opt-in, and the opt-in belongs next to the thing it describes.
inline auto visual_parts(const Open& p) {
    return std::make_tuple(visual::ref(p.picker));
}

} // namespace mention

namespace visual {
template <> inline constexpr bool trusted_parts<mention::Open> = true;
} // namespace visual

using MentionState = std::variant<mention::Closed, mention::Open>;

[[nodiscard]] inline bool mention_palette_is_open(const MentionState& s) noexcept {
    return std::holds_alternative<mention::Open>(s);
}
[[nodiscard]] inline       mention::Open* mention_palette_opened(MentionState& s)       noexcept { return std::get_if<mention::Open>(&s); }
[[nodiscard]] inline const mention::Open* mention_palette_opened(const MentionState& s) noexcept { return std::get_if<mention::Open>(&s); }

} // namespace agentty
