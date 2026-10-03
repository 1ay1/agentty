// Regression test for the `cmd` shell-tool alias canonification
// (agentty/runtime/app/update/stream_args.hpp).
//
// Reproduces the discrepancy observed with Laguna S 2.1: the model emits
// the shell tool's command parameter as `cmd` (OpenAI-style spec shape)
// instead of the schema's `command`. mcp-cpp's dispatcher ArgReader
// already accepts `cmd` via its alias table — but the host-side
// required-field guard (missing_required_field) and every UI surface
// (permission card, output panel, timeline) read `tc.args["command"]`
// literally, so the call died with "missing the required field
// `command`" before dispatch. The fix canonifies `cmd` → `command` at
// each args-parse site and makes the guard alias-aware.
//
// Standalone (no maya / no runtime) — compiles against the header +
// nlohmann + spec, mirroring param_tag_repair_test.cpp.

#include "agtest.hpp"

#include "agentty/runtime/app/update/stream_args.hpp"

#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

using json = nlohmann::json;
using agentty::app::detail::canonify_alias_keys;
using agentty::app::detail::canonify_tool_args;
using agentty::app::detail::kCommandAliases;
using agentty::app::detail::missing_required_field;

namespace {
void expect(const std::string& name, bool cond, const std::string& detail = {}) {
    std::string msg = detail.empty() ? name : name + " — " + detail;
    CHECK_MESSAGE(cond, msg);
}
} // namespace

TEST_CASE("shell cmd alias canonification") {
    // ── Case 1: the exact shape seen on the wire (Laguna S 2.1) ─────────
    // {"cmd": "cat /etc/os-release …"} — parses fine, then the old guard
    // failed it with "missing the required field `command`".
    {
        json args = {{"cmd", "cat /etc/os-release 2>/dev/null | head -3"}};
        // Canonify first (as the parse sites now do), then guard.
        (void)canonify_tool_args("shell", args);
        auto missing = missing_required_field("shell", args);
        expect("cmd-shaped call passes the required-field guard",
               missing.empty(),
               "missing_required_field returned `" + std::string{missing} + "`");
        expect("canonical key materialised", args.contains("command"));
        expect("canonical value preserved",
               args["command"].get<std::string>()
                   == "cat /etc/os-release 2>/dev/null | head -3");
        expect("alias entry kept so retry sees what was sent",
               args.contains("cmd"));
    }

    // ── Case 2: every accepted alias, canonified ────────────────────────
    {
        for (auto alias : kCommandAliases) {
            if (alias == "command") continue;
            json args = json::object();
            args[std::string{alias}] = "echo alias-ok";
            (void)canonify_tool_args("shell", args);
            auto missing = missing_required_field("shell", args);
            std::string an{alias};
            expect(an + " accepted by the guard",
                   missing.empty(),
                   "missing_required_field returned `" + std::string{missing}
                       + "`");
            expect(an + " canonified",
                   args.contains("command")
                       && args["command"].get<std::string>() == "echo alias-ok");
        }
    }

    // ── Case 3: canonical key already present wins ──────────────────────
    // Both keys sent (rare, but a proxy could merge chunks): the canonical
    // value must never be overwritten by the alias.
    {
        json args = {{"command", "echo canonical"},
                     {"cmd",     "echo alias"}};
        bool changed = canonify_alias_keys(args, "command", kCommandAliases);
        expect("no rename when canonical key present", !changed);
        expect("canonical value untouched",
               args["command"].get<std::string>() == "echo canonical");
        expect("guard passes", missing_required_field("shell", args).empty());
    }

    // ── Case 4: genuinely missing → still fails, unchanged ──────────────
    // The guard is an alias extension, not a weakening of the required
    // field: a shell call with NO command variant still reports the error.
    {
        json args = {{"display_description", "no command here"}};
        auto missing = missing_required_field("shell", args);
        expect("missing command still reported",
               missing == "command",
               "returned `" + std::string{missing} + "`");
        (void)canonify_tool_args("shell", args);
        expect("canonify is a no-op with no alias key",
               !args.contains("command"));
    }

    // ── Case 5: non-string alias value is not canonified ────────────────
    // The guard's alias check accepts only nonempty strings; canonify
    // matches that contract so a numeric `cmd` can't smuggle through as
    // an "empty" command (the dispatcher would then coerce-dump it).
    {
        json args = {{"cmd", 42}};
        bool changed = canonify_alias_keys(args, "command", kCommandAliases);
        expect("non-string alias not renamed", !changed);
        expect("missing still reported",
               missing_required_field("shell", args) == "command");
    }

    // ── Case 6: tool scoping — other tools are untouched ────────────────
    {
        json read_args = {{"cmd", "should stay"}};
        expect("canonify is inert for non-command tools",
               !canonify_tool_args("read", read_args));
        expect("read args unchanged",
               !read_args.contains("command"));
        json empty = json::object();
        expect("no crash on empty object",
               !canonify_tool_args("shell", empty));
    }

    // ── Case 7: diagnostics / test / process_start share the fix ────────
    {
        for (std::string_view tn : {"diagnostics", "test", "process_start"}) {
            json args = {{"cmd", "echo shared"}};
            (void)canonify_tool_args(tn, args);
            auto missing = missing_required_field(tn, args);
            std::string tn_s{tn};
            expect(tn_s + " cmd-shaped call passes the guard",
                   missing.empty(),
                   "missing_required_field returned `" + std::string{missing}
                       + "`");
            expect(tn_s + " canonified",
                   args["command"].get<std::string>() == "echo shared");
        }
    }

    // ── Case 8: the alias list stays SHORT, and deliberately so ────────
    //
    // Every entry is a key whose value gets handed to a shell, so a wrong
    // guess does not cost a failed call -- it executes the wrong string.
    // `shell` and `run` were in the first version of this list and are out:
    // `shell` as a key on a tool NAMED shell is ambiguous rather than a
    // spelling of "the command", and `run` is a real agentty subcommand a
    // model could send meaning something else (notably to process_start).
    //
    // Pinned as a test because the failure mode of re-adding them is silent
    // and bad: the call succeeds, running a string nobody intended.
    {
        for (auto alias : kCommandAliases) {
            expect("no ambiguous alias in the command family",
                   alias != "shell" && alias != "run",
                   "`" + std::string{alias} + "` is a key whose value we hand "
                   "to a shell; it must be an unambiguous spelling of "
                   "\"the command to run\"");
        }

        // And a call that sends ONLY an unaccepted key must still fail the
        // guard loudly, rather than be silently promoted to a command.
        json args = {{"shell", "rm -rf /"}};
        (void)canonify_tool_args("shell", args);
        expect("an unaccepted key is NOT promoted to command",
               !args.contains("command"));
        expect("and the guard still rejects the call",
               missing_required_field("shell", args) == "command",
               "a key we do not understand must fail loudly, not execute");
    }

    // ── Case 9: canonify is IDEMPOTENT ───────────────────────────────
    //
    // It now runs at the stream-parse sites AND again at the dispatch
    // boundary (mcp_tools_bridge's def.execute), so args are canonified
    // twice on the normal path. The second pass must be a no-op: the first
    // one leaves `command` present, which is exactly the case
    // canonify_alias_keys refuses to touch.
    {
        json args = {{"cmd", "echo once"}};
        const bool first = canonify_tool_args("shell", args);
        const bool second = canonify_tool_args("shell", args);
        expect("first pass canonifies", first);
        expect("second pass is a no-op", !second);
        expect("value survives both passes",
               args["command"].get<std::string>() == "echo once");
        expect("original key is still there for a model re-reading its call",
               args["cmd"].get<std::string>() == "echo once");
    }
}

TEST_CASE("edit top-level old_text/new_text guard acceptance") {
    // Regression for the second Laguna S 2.1 finding (do_edit.md trace).
    // The edit tool's edits[] entries are keyed old_text/new_text — the
    // schema's OWN vocabulary — and the model leaks that spelling into the
    // top-level old_string/new_string slots. parse_edit_args accepts both
    // spellings at both levels, so the call was well-formed and would have
    // executed; the guard failed it with "missing the required field
    // `old_string`" because kOldStrAliases/kNewStrAliases didn't list the
    // _text forms. Unlike the shell fix, NO canonification is involved:
    // every edit surface (guard, preview sniffer, edit_body, the ACP diff
    // card) already reads both spellings, and widening the guard alone
    // makes the whole path work.

    // ── Case 1: the exact shape reconstructed from the wire trace ──────
    // Streamed fragments: {"path": …, "old_text": …, "new_text": …,
    // "display_description": …} (the logged tail starts at `, "new_text":`).
    {
        json args = {{"path", "~/.config/yazi/superuser.yazi/main.lua"},
                     {"old_text", "local sudo = \"sudo\""},
                     {"new_text", "local sudo = {\"sudo\", \"sudo-rs\", \"run0\", \"doas\"}"},
                     {"display_description",
                      "Add -S flag to sudo/sudo-rs and mark stdin-capable escalators"}};
        auto missing = missing_required_field("edit", args);
        expect("top-level old_text/new_text call passes the guard",
               missing.empty(),
               "missing_required_field returned `" + std::string{missing} + "`");
    }

    // ── Case 2: every spelling pair the guard now accepts ───────────────
    {
        struct Spelling { const char* oldk; const char* newk; };
        const Spelling spellings[] = {{"old_string", "new_string"},
                                      {"old_text",   "new_text"},
                                      {"old_str",    "new_str"},
                                      {"oldStr",     "newStr"}};
        for (const auto& sp : spellings) {
            json args = {{"path", "a.lua"},
                         {sp.oldk, "x"},
                         {sp.newk, "y"}};
            auto missing = missing_required_field("edit", args);
            expect(std::string{sp.oldk} + "/" + std::string{sp.newk}
                       + " accepted by the edit guard",
                   missing.empty(),
                   "returned `" + std::string{missing} + "`");
        }
    }

    // ── Case 3: mixed spellings accepted — each field checked against
    //    its own table independently ─────────────────────────────────
    {
        json args = {{"path", "a.lua"},
                     {"old_text", "x"},
                     {"new_string", "y"}};
        expect("mixed old_text + new_string accepted",
               missing_required_field("edit", args).empty());
        json args2 = {{"path", "a.lua"},
                      {"old_string", "x"},
                      {"new_text", "y"}};
        expect("mixed old_string + new_text accepted",
               missing_required_field("edit", args2).empty());
    }

    // ── Case 4: edits[] short-circuit behaviour is preserved ────────────
    // A non-empty edits[] array satisfies the guard before the top-level
    // old/new check ever runs.
    {
        json args = {{"path", "a.lua"},
                     {"edits", json::array({
                         json{{"old_text", "x"}, {"new_text", "y"}}})}};
        expect("edits[] call passes", missing_required_field("edit", args).empty());
        json empty_edits = {{"path", "a.lua"},
                            {"edits", json::array()}};
        auto missing = missing_required_field("edit", empty_edits);
        expect("empty edits[] falls through to top-level check",
               missing == "old_string",
               "returned `" + std::string{missing} + "`");
    }

    // ── Case 5: non-evidenced synonyms stay out ─────────────────────────
    // Policy (see the table comment in stream_args.hpp): add an alias only
    // with a transcript showing a model actually emitting it. None of these
    // has one, so they must keep failing — a wrong `old` guess on the edit
    // path does not cost a failed call, it mutates the wrong text.
    {
        json args = {{"path", "a.lua"},
                     {"old", "x"},
                     {"new", "y"}};
        expect("bare old/new NOT accepted (unevidenced)",
               missing_required_field("edit", args) == "old_string");
    }

    // ── Case 6: genuinely missing still fails, unchanged ─────────────────
    {
        json args = {{"path", "a.lua"}, {"new_text", "y"}};
        auto missing = missing_required_field("edit", args);
        expect("missing old_string still reported",
               missing == "old_string",
               "returned `" + std::string{missing} + "`");
        json args2 = {{"path", "a.lua"}, {"old_text", "x"}};
        auto missing2 = missing_required_field("edit", args2);
        expect("missing new_string still reported",
               missing2 == "new_string",
               "returned `" + std::string{missing2} + "`");
    }
}