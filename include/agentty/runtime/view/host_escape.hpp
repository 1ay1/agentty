#pragma once
// agentty::ui::host — cooperating-host (editor) integration escapes.
//
// When agentty's TUI runs on an editor's PTY (Emacs/vterm, and any terminal
// that watches for our private OSC), it can push "the agent just touched this
// file" hints so the editor follows along natively — opening the file, moving
// point, showing a diff — instead of the user hunting for what changed.
//
// The channel is a single private OSC, emitted via maya's frame-safe
// Cmd::emit_osc (out-of-band with rendering, cursor-neutral):
//
//     OSC 5379 ; agentty ; <json> ST
//
// with <json> = {"event":"<kind>", ...fields}. 5379 is a private code no
// mainstream terminal claims; the "agentty;" tag lets a host match ours
// unambiguously and ignore everything else. Hosts that don't watch for it see
// an inert OSC and drop it — zero cost, zero corruption.
//
// This layer is PURE: it only builds strings + reports whether a cooperating
// host is present. The reducer decides when to emit and wraps the result in a
// maya Cmd. Gated so a normal terminal session pays nothing.

#include <optional>
#include <string>
#include <string_view>

namespace agentty::ui::host {

// Is agentty running under a cooperating editor host that wants integration
// escapes? True when AGENTTY_HOST names a known host (currently "emacs"), or
// as a fallback when $INSIDE_EMACS is set with a vterm marker. Sampled once
// and cached (the environment doesn't change mid-process).
[[nodiscard]] bool integration_active();

// The private OSC that tells the host a file tool just acted on `path`
// (absolute, forward-slash) at an optional 1-based `line`. `kind` is the tool
// name ("read" | "edit" | "write" | "move" | …) so the host can choose the
// gesture (open vs. diff vs. reveal). Returns nullopt when integration is
// inactive or `path` is empty — the caller emits nothing.
[[nodiscard]] std::optional<std::string>
file_event_osc(std::string_view kind, std::string_view path,
               std::optional<int> line = std::nullopt);

} // namespace agentty::ui::host
