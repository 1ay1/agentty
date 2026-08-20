// palette_render_probe — renders the command palette and asserts its layout
// invariants: a TREE hierarchy on the empty query (┌─ header, │ spine, └
// close), a flat list when filtering, gating hides dead rows, and UTF-8-clean
// truncation. Lives in the fold suite so it links the full runtime object
// tree and renders through the real maya path.
#include "agentty/runtime/model.hpp"
#include "agentty/runtime/view/pickers.hpp"
#include "agentty/runtime/command_palette.hpp"
#include <maya/app/inline.hpp>
#include <cstdio>
#include <string>

using namespace agentty;

static int g_fail = 0;
static void check(bool ok, const char* what) {
    if (!ok) { std::printf("  FAIL: %s\n", what); ++g_fail; }
}
static std::string render(const Model& m) {
    return maya::render_to_string(ui::command_palette(m), 82);
}
static bool has(const std::string& h, const std::string& n) {
    return h.find(n) != std::string::npos;
}
static bool has_mojibake(const std::string& s) {         // U+FFFD = split codepoint
    return s.find("\xef\xbf\xbd") != std::string::npos;
}

int main() {
    Model m;
    m.ui.command_palette = palette::Open{};
    m.d.pending_changes.push_back(FileChange{});   // so the Changes section shows

    // ── empty query: a real TREE (bracket header + spine + close) ─────────
    {
        std::string out = render(m);
        check(has(out, "\xe2\x94\x8c\xe2\x94\x80 THREAD"),   // ┌─ THREAD
              "empty query renders a bracket-connector section header");
        check(has(out, "\xe2\x94\x82"),                     // │ spine
              "group members carry a vertical spine");
        check(has(out, "\xe2\x94\x94"),                     // └ close
              "the last member of a group closes the bracket");
        check(has(out, "New thread"), "commands render under their section");
        check(!has_mojibake(out), "no split-codepoint mojibake (empty)");
    }

    // ── filtered: tree gone, flat list, labels intact ────────────────────
    {
        auto* o = std::get_if<palette::Open>(&m.ui.command_palette);
        o->query = "th";
        std::string out = render(m);
        check(!has(out, "\xe2\x94\x8c\xe2\x94\x80 THREAD"),
              "typing collapses the tree to a flat list");
        check(has(out, "New thread") && has(out, "Fork thread"),
              "filtered rows keep whole labels");
        check(!has_mojibake(out), "no split-codepoint mojibake (filtered)");

        o->query = "changes";
        std::string ch = render(m);
        check(has(ch, "Review changes") && has(ch, "Accept all")
              && has(ch, "Reject all"),
              "\"changes\" surfaces the whole Changes category");
    }

    // ── gating: no pending diff hides the Changes commands ────────────────
    {
        Model m2;
        m2.ui.command_palette = palette::Open{};   // no pending_changes
        std::string out = render(m2);
        check(!has(out, "Accept all changes"), "no pending diff hides Accept-all");
        check(has(out, "New thread"), "unrelated commands still shown");
    }

    if (g_fail == 0) std::puts("palette_render_probe: OK");
    return g_fail ? 1 : 0;
}
