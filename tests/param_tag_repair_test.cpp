// Regression test for the XML-in-JSON tool-call recovery
// (agentty/runtime/app/update/param_tag_repair.hpp).
//
// Reproduces the failure mode observed in real sessions: the model mixes
// Anthropic's internal `<parameter name="…">` tool-call syntax into the
// JSON tool input, producing syntactically-valid-but-wrong args where the
// required field is buried inside a stray string value. Standalone (no
// maya / no runtime) — compiles against the header + nlohmann + spec.

#include "agentty/runtime/app/update/param_tag_repair.hpp"

#include <cstdio>
#include <string>

#include <nlohmann/json.hpp>

using json = nlohmann::json;
using agentty::app::detail::repair_param_tag_leak;

namespace {
int failures = 0;
int total    = 0;
void check(const char* name, bool cond, const std::string& detail = {}) {
    ++total;
    if (cond) std::printf("  ok   %s\n", name);
    else { ++failures; std::printf("  FAIL %s — %s\n", name, detail.c_str()); }
}
} // namespace

int main() {
    // ── Case 1: the exact shape seen on the wire ──────────────────────
    // {"path","display_description","edit":"\n<parameter name=\"old_text\">OLD",
    //  "new_text":"NEW"}  — note `edit` (singular) and old_text hidden inside.
    {
        json args = {
            {"display_description", "probe edit"},
            {"edit", "\n<parameter name=\"old_text\">    constexpr int k = 1;\n"},
            {"new_text", "    constexpr int k = 2;\n"},
            {"path", "src/foo.cpp"},
        };
        bool changed = repair_param_tag_leak("edit", args);
        check("case1: repaired", changed, args.dump());
        check("case1: edits array present",
              args.contains("edits") && args["edits"].is_array()
                  && args["edits"].size() == 1, args.dump());
        check("case1: stray `edit` key removed", !args.contains("edit"));
        if (args.contains("edits") && !args["edits"].empty()) {
            auto& e = args["edits"][0];
            check("case1: old_text recovered byte-exact",
                  e.value("old_text", "") == "    constexpr int k = 1;\n",
                  e.value("old_text", ""));
            check("case1: new_text from clean key",
                  e.value("new_text", "") == "    constexpr int k = 2;\n",
                  e.value("new_text", ""));
        }
        check("case1: path preserved", args.value("path", "") == "src/foo.cpp");
        check("case1: display_description preserved",
              args.value("display_description", "") == "probe edit");
    }

    // ── Case 2: both old & new smuggled into one string, with closers ──
    {
        json args = {
            {"path", "a.cpp"},
            {"edits", "<parameter name=\"old_text\">AAA</parameter>"
                      "<parameter name=\"new_text\">BBB</parameter>"},
        };
        bool changed = repair_param_tag_leak("edit", args);
        check("case2: repaired", changed, args.dump());
        if (args.contains("edits") && args["edits"].is_array()
            && !args["edits"].empty()) {
            auto& e = args["edits"][0];
            check("case2: old_text", e.value("old_text", "") == "AAA");
            check("case2: new_text", e.value("new_text", "") == "BBB");
        } else {
            check("case2: edits is array", false, args.dump());
        }
    }

    // ── Case 3: no marker → untouched, returns false ──────────────────
    {
        json args = {
            {"path", "a.cpp"},
            {"edits", json::array({ json{{"old_text","X"},{"new_text","Y"}} })},
        };
        json before = args;
        bool changed = repair_param_tag_leak("edit", args);
        check("case3: not repaired", !changed);
        check("case3: unchanged", args == before, args.dump());
    }

    // ── Case 4: write leak — content smuggled into a stray key ─────────
    {
        json args = {
            {"path", "new.txt"},
            {"write", "<parameter name=\"content\">hello\nworld\n"},
        };
        bool changed = repair_param_tag_leak("write", args);
        check("case4: repaired", changed, args.dump());
        check("case4: content recovered",
              args.value("content", "") == "hello\nworld\n",
              args.value("content", ""));
        check("case4: path preserved", args.value("path", "") == "new.txt");
    }

    // ── Case 5: a well-formed edit whose new_text legitimately contains
    //    the literal marker text. extract_param_tags only scans TOP-LEVEL
    //    string values, so a marker nested inside the `edits` array (a
    //    non-string value at top level) is invisible — repair is a no-op
    //    and the genuine old_text survives. This, plus the caller gating
    //    on the already-failing path, is why editing a file that contains
    //    the marker (like this one) is never corrupted.
    {
        json args = {
            {"path", "a.cpp"},
            {"edits", json::array({ json{
                {"old_text","real old"},
                {"new_text","// see <parameter name=\"old_text\"> docs"}}})},
        };
        // Directly exercising repair: it should still produce a usable
        // edit with the genuine top-level old_text preferred.
        repair_param_tag_leak("edit", args);
        bool usable = args.contains("edits") && args["edits"].is_array()
                      && !args["edits"].empty()
                      && args["edits"][0].value("old_text","") == "real old";
        check("case5: genuine old_text preferred over in-content marker",
              usable, args.dump());
    }

    std::printf("\n%d/%d checks passed\n", total - failures, total);
    return failures ? 1 : 0;
}
