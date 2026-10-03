#pragma once
// agentty::app::detail — shared leaf helpers for the streaming-tool-args
// machinery. These are pure functions over (json / std::string / std::span)
// with no Model or reducer dependency; they were previously private to
// stream.cpp but are now used by BOTH the live-preview decoder
// (stream_preview.cpp) and the finalize/salvage path (stream.cpp), so they
// live here to avoid duplication or an ODR clash.
//
// `inline` (not `static`) so the two TUs share one definition — the address
// is irrelevant (nobody takes their pointer) and inlining keeps the hot
// preview path allocation-free.

#include <optional>
#include <span>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

#include "agentty/tool/spec.hpp"
#include "agentty/tool/util/partial_json.hpp"

namespace agentty::app::detail {

// Keys models sometimes emit in place of our canonical field name.
//
// These predate the command family below and were described as mirroring
// mcp-cpp's ArgReader. They do not: ArgReader has no alias mechanism (see
// the note on kCommandAliases). A few of these strings happen to appear in
// mcp-cpp for unrelated reasons -- `file_path` in cap/scheduler.hpp's own
// take() list -- which is what made the claim look plausible. Treat every
// list here as agentty's own host-side tolerance, honoured by canonify and
// by salvage_args, and by nothing downstream.
//
// WIDENING ONE OF THESE HAS TWO CONSUMERS, not one. The required-field guard
// only READS the table (an alias satisfies the check and nothing is rewritten),
// but salvage_args CANONIFIES through it -- `pick("old_string", kOldStrAliases)`
// copies the first alias it finds onto the canonical key. That is safe today
// for a reason worth writing down rather than rediscovering: salvage only runs
// on a TRUNCATED stream, ended_inside_string() refuses the mid-string case
// first, and every dispatcher reads both spellings anyway, so the rewrite is a
// no-op in effect. Add an alias whose canonical twin is read literally
// somewhere and that stops being true.
inline constexpr std::string_view kPathAliases[]    = {"path", "file_path", "filepath", "filename"};
// old_text/new_text are transcript-evidenced (Laguna S 2.1, agentty session
// of 2026-10-03, do_edit.md trace): the edit tool's edits[] entries are keyed
// old_text/new_text (see edit_schema), and the model leaks that spelling into
// the TOP-LEVEL old_string/new_string slots. parse_edit_args accepts both
// spellings at both levels (top level probes `old_string` then `old_text`;
// edits[] probes the reverse), so dispatch was never the failure — the guard
// was. `old`/`search`/`find`/`from` are deliberately NOT listed: no transcript
// shows a model emitting them, and unlike the _text forms they are not the
// schema's own vocabulary for this tool.
inline constexpr std::string_view kOldStrAliases[]  = {"old_string", "old_text",
                                                       "old_str", "oldStr"};
inline constexpr std::string_view kNewStrAliases[]  = {"new_string", "new_text",
                                                       "new_str", "newStr"};
inline constexpr std::string_view kContentAliases[] = {"content", "file_text", "text",
                                                        "file_content", "contents",
                                                        "body", "data"};
// Keys models emit for the shell tool's `command` parameter. Laguna S 2.1
// (and models cross-trained on OpenAI-style specs) reliably send `cmd`.
//
// NOT mirrored from mcp-cpp's ArgReader, despite what the comment above says
// about the older lists. ArgReader is a plain keyed reader -- str(key),
// require_str(key), raw(key) -- with no alias table at all; the only "cmd"
// anywhere in mcp-cpp is cmd.exe detection in cap/process.hpp. So
// canonification here is not a convenience that duplicates dispatch, it is
// THE mechanism: relax the guard without canonifying and a `cmd`-shaped call
// stops failing loudly and starts reaching a dispatcher that reads `command`,
// finds nothing, and runs an empty string. Guard and canonify ship together
// or not at all.
//
// Kept deliberately SHORT. Every entry is a key whose value we will hand to a
// shell, so a wrong guess does not cost a failed call -- it executes the wrong
// string. `shell` and `run` were dropped for that reason: `shell` as a key on
// a tool NAMED shell is ambiguous rather than a spelling of "the command",
// and `run` is a real agentty subcommand that a model could plausibly send
// meaning something else (notably to process_start). Add an alias only with a
// transcript showing a model actually emitting it.
inline constexpr std::string_view kCommandAliases[] = {"command", "cmd",
                                                       "shell_command",
                                                       "script", "cmdline"};
inline constexpr std::string_view kDisplayDescription = "display_description";

// Hard cap on the live content preview shown during streaming. The widget
// only renders the first ~6 lines of `content` while the model is mid-write;
// re-extracting / re-laying-out a multi-KB body 8x/sec was what made big
// writes "feel" stuck even when bytes were arriving. 4 KiB covers ~50 wide
// lines — far more than the widget shows — and bounds per-tick work.
inline constexpr std::size_t kStreamingPreviewCap = 4 * 1024;

// Try each alias key in turn, returning the first field that sniffs out of the
// raw streaming buffer. `partial` selects the progressive sniffer (tolerates
// an unclosed value) vs the strict one.
[[nodiscard]] inline std::optional<std::string>
sniff_any(const std::string& raw,
          std::span<const std::string_view> keys,
          bool partial) {
    for (auto k : keys) {
        auto v = partial ? agentty::tools::util::sniff_string_progressive(raw, k)
                         : agentty::tools::util::sniff_string(raw, k);
        if (v) return v;
    }
    return std::nullopt;
}

// Rename alias keys to their canonical name IN PLACE in a parsed args
// object: for the first alias of `canon` present in the object (with
// `canon` itself absent), copy the value under `canon`. The original
// entry stays so a model re-reading its own call still finds what it sent.
// Returns true when any rename happened (callers use it to mark_args_dirty).
[[nodiscard]] inline bool canonify_alias_keys(nlohmann::json& args,
                                              std::string_view canon,
                                              std::span<const std::string_view> keys) {
    if (!args.is_object()) return false;
    bool changed = false;
    for (auto k : keys) {
        if (k == canon) continue;
        auto it = args.find(std::string{k});
        if (it == args.end() || !it->is_string()) continue;
        if (args.contains(std::string{canon})) break;   // canonical wins
        args[std::string{canon}] = *it;                 // copy: alias entry stays intact
        changed = true;
    }
    return changed;
}

// Canonify the alias keys the given built-in tool is known to receive into
// their canonical names, in place. Currently covers the `command` family
// (shell / diagnostics / test / process_start): Laguna S 2.1 and models
// cross-trained on OpenAI-style specs emit `cmd` for shell, which the
// dispatcher's ArgReader accepts but every host-side consumer reads
// literally. Returns true when anything was renamed.
[[nodiscard]] inline bool canonify_tool_args(std::string_view tool_name,
                                             nlohmann::json& args) {
    if (tool_name == "shell" || tool_name == "diagnostics"
        || tool_name == "test" || tool_name == "process_start")
        return canonify_alias_keys(args, "command", kCommandAliases);
    return false;
}

// Attempt to parse the streaming buffer via the partial-JSON closer. Returns
// an object when the result is a parseable object, otherwise nullopt. Strictly
// more capable than the regex sniffer — handles nested objects
// (edits[].old_text) and escaped quotes — but callers still fall back to the
// sniffer for fields the partial closer can't yet expose (e.g. when the
// current field's value is a partial string that won't close until later).
[[nodiscard]] inline std::optional<nlohmann::json>
try_parse_partial(const std::string& raw) {
    if (raw.empty()) return std::nullopt;
    try {
        auto closed = agentty::tools::util::close_partial_json(raw);
        auto parsed = nlohmann::json::parse(closed, /*cb=*/nullptr,
                                            /*allow_exceptions=*/false);
        if (parsed.is_discarded() || !parsed.is_object()) return std::nullopt;
        return parsed;
    } catch (...) {
        return std::nullopt;
    }
}

// First alias key present as a string in a (partial-parsed) object.
[[nodiscard]] inline std::optional<std::string>
get_string_any(const nlohmann::json& obj,
               std::span<const std::string_view> keys) {
    for (auto k : keys) {
        auto it = obj.find(std::string{k});
        if (it == obj.end()) continue;
        if (it->is_string()) return it->get<std::string>();
    }
    return std::nullopt;
}

// Truncation guard: after the stream parses/salvages tool args, verify the
// minimum fields the target tool actually needs. A common failure mode is the
// wire dropping between `display_description`'s closing `"` and the
// `"content":` that should follow — close_partial_json then strips the
// dangling `,` and produces a well-formed but content-less object. Running the
// tool on that would silently produce an empty file and the model would retry
// on a cryptic "content required" loop. Returns the name of the first missing
// required field, or {} when all present (or the tool has none / is unknown).
[[nodiscard]] inline std::string_view
missing_required_field(std::string_view tool_name, const nlohmann::json& args) {
    if (!args.is_object()) return "(args)";
    auto is_nonempty_string_any = [&](std::span<const std::string_view> keys) {
        for (auto k : keys) {
            auto it = args.find(std::string{k});
            if (it == args.end() || !it->is_string()) continue;
            if (!it->get_ref<const std::string&>().empty()) return true;
        }
        return false;
    };
    auto is_nonempty_string = [&](std::string_view key) {
        auto it = args.find(std::string{key});
        return it != args.end() && it->is_string()
            && !it->get_ref<const std::string&>().empty();
    };

    // Closed-set dispatch via spec::Kind. Tools not in the catalog
    // (kind_of returns nullopt) get treated as "no required fields" so an
    // unknown future tool isn't blocked by this guard — the runtime
    // dispatcher will reject the unknown name with a typed error first.
    auto kind = tools::spec::kind_of(tool_name);
    if (!kind) return {};

    using K = tools::spec::Kind;
    switch (*kind) {
        case K::Write:
            if (!is_nonempty_string_any(kPathAliases))    return "path";
            if (!is_nonempty_string_any(kContentAliases)) return "content";
            return {};
        case K::Edit: {
            if (!is_nonempty_string_any(kPathAliases))    return "path";
            auto it = args.find("edits");
            if (it != args.end() && it->is_array() && !it->empty()) return {};
            if (!is_nonempty_string_any(kOldStrAliases))  return "old_string";
            if (!is_nonempty_string_any(kNewStrAliases))  return "new_string";
            return {};
        }
        case K::Bash:
        case K::Diagnostics:
        case K::ProcessStart:
            if (!is_nonempty_string_any(kCommandAliases)) return "command";
            return {};
        case K::ProcessPoll:
        case K::ProcessStop:
            if (!is_nonempty_string("id")) return "id";
            return {};
        case K::Move:
            if (!is_nonempty_string("source")) return "source";
            if (!is_nonempty_string("destination")) return "destination";
            return {};
        case K::Remove:
            if (!is_nonempty_string_any(kPathAliases)) return "path";
            return {};
        case K::Grep:
            if (!is_nonempty_string("pattern")) return "pattern";
            return {};
        case K::FindDefinition:
            if (!is_nonempty_string("symbol")) return "symbol";
            return {};
        case K::SearchDocs:
            if (!is_nonempty_string("query")) return "query";
            return {};
        case K::WebFetch:
            if (!is_nonempty_string("url")) return "url";
            return {};
        case K::GitCommit:
            if (!is_nonempty_string("message")) return "message";
            return {};
        case K::GitBlame:
            if (!is_nonempty_string_any(kPathAliases)) return "path";
            return {};
        case K::Remember:
            // remember requires the `text` body. `scope` is optional.
            if (!is_nonempty_string("text")) return "text";
            return {};
        case K::Forget:
            // forget accepts either `id` or `substring` — the tool's own
            // parser surfaces the "need one of" error with a richer message
            // than this guard could, so don't gate here.
            return {};
        case K::Wipe:
            // wipe_memory requires `scope`, but the tool's own parser
            // produces a richer error message than this guard could. Defer
            // the gate to the tool layer; the `confirm` two-step protects
            // against accidental wipes regardless.
            return {};
        case K::Task:
            // task requires a `prompt`/`description` for the subagent.
            if (!is_nonempty_string("prompt")) return "prompt";
            return {};
        case K::Skill:
            // skill requires the `name` of the skill to load.
            if (!is_nonempty_string("name")) return "name";
            return {};
        // `path` is nice-to-have but not strictly required for these
        // (list_dir/glob default to cwd; read without path is already a tool
        // error — surfacing it from the tool itself preserves the typed
        // ToolError chain instead of converting to a stream-level salvage
        // failure here).
        case K::Read:
        case K::ListDir:
        case K::Glob:
        case K::GitDiff:
        case K::GitLog:
        case K::GitStatus:
        case K::GitShow:
        case K::GitBranch:   // `name` requirement is action-dependent; the
                             // tool's own parser gives the richer error.
        case K::GitStash:    // action-gated; parser validates per-action.
        case K::GitRebase:   // upstream required only for action=onto.
        case K::GitCherryPick: // commits required only for action=pick.
        case K::Test:
        case K::WebSearch:
        case K::RepoMap:
        case K::SearchCode:
        case K::Todo:
            return {};
    }
    // No default: — switch is exhaustive over Kind, so a new tool Kind
    // re-triggers -Wswitch here (a build error under -Werror) rather than
    // silently falling through to "no required fields". DESIGN.md rule.
    return {};   // unreachable
}

} // namespace agentty::app::detail
