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

// Keys models sometimes emit in place of our canonical field name. Mirrors
// the ArgReader alias table — keep in sync.
inline constexpr std::string_view kPathAliases[]    = {"path", "file_path", "filepath", "filename"};
inline constexpr std::string_view kOldStrAliases[]  = {"old_string", "old_str", "oldStr"};
inline constexpr std::string_view kNewStrAliases[]  = {"new_string", "new_str", "newStr"};
inline constexpr std::string_view kContentAliases[] = {"content", "file_text", "text",
                                                        "file_content", "contents",
                                                        "body", "data"};
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
            if (!is_nonempty_string("command")) return "command";
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
        case K::WebSearch:
        case K::Todo:
            return {};
    }
    return {};   // unreachable: switch is exhaustive over Kind
}

} // namespace agentty::app::detail
