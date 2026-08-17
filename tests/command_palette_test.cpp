// command_palette_test — locks the Ctrl+K palette's row catalog + filter UX.
//
// The palette is pure data (kCommands) + one pure function (filtered_commands),
// so it's cheaply unit-testable without any UI/runtime. This pins the two
// invariants that keep every row working and discoverable:
//   1. CATALOG COMPLETENESS — every Command enum value has exactly one row.
//      A missing row = a dead palette entry; the dispatcher switch would still
//      compile but the command could never be selected.
//   2. FILTER UX — matching spans label + description + shortcut (discovery by
//      intent), and label hits rank above description-only hits.

#include "agtest.hpp"

#include "agentty/runtime/command_palette.hpp"

#include <array>
#include <string_view>

using namespace agentty;


// Every enum value, so we can assert the catalog covers each exactly once.
static constexpr std::array kAll = {
    Command::NewThread, Command::ReviewChanges, Command::AcceptAll,
    Command::RejectAll, Command::CycleProfile, Command::OpenModels,
    Command::OpenProviders, Command::OpenThreads, Command::OpenPlan,
    Command::RunCodeBlock, Command::InspectToolOutputs, Command::CompactContext,
    Command::SmartMode,
    Command::ResetSmartLearning,
    Command::RewindCheckpoint, Command::ForkThread,
    Command::OpenPlugins, Command::OpenCommands, Command::OpenAgents, Command::OpenHooks,
    Command::OpenRagSettings, Command::OpenLogin,
    Command::SignOut, Command::Quit,
};

static bool has_id(const std::vector<const CommandDef*>& v, Command id) {
    for (const auto* c : v) if (c->id == id) return true;
    return false;
}

TEST_CASE("command palette") {
    // ── 1. catalog completeness (enum ⇄ kCommands bijection) ──────────────
    check(kCommands.size() == kAll.size(),
          "kCommands has exactly one row per Command enum value");
    for (Command id : kAll) {
        int n = 0;
        for (const auto& c : kCommands) if (c.id == id) ++n;
        check(n == 1, "each Command id appears exactly once in kCommands");
    }
    // Every row has a non-empty label + description (no blank palette rows).
    for (const auto& c : kCommands) {
        check(c.label && *c.label, "row has a label");
        check(c.description && *c.description, "row has a description");
        check(c.shortcut != nullptr, "row has a (possibly empty) shortcut");
    }

    // ── 2. empty query returns every row, in catalog order ────────────────
    {
        auto all = filtered_commands("");
        check(all.size() == kCommands.size(), "empty query returns all rows");
        bool ordered = true;
        for (std::size_t i = 0; i < all.size(); ++i)
            if (all[i]->id != kCommands[i].id) ordered = false;
        check(ordered, "empty query preserves catalog order");
    }

    // ── 3. label match ────────────────────────────────────────────────────
    {
        auto r = filtered_commands("thread");
        check(has_id(r, Command::NewThread) && has_id(r, Command::OpenThreads),
              "\"thread\" matches New thread + Open threads");
    }

    // ── 4. DESCRIPTION match (discovery by intent) ────────────────────────
    {
        // "diff" is only in Review changes' DESCRIPTION, not its label.
        auto r = filtered_commands("diff");
        check(has_id(r, Command::ReviewChanges),
              "\"diff\" finds Review changes via its description");
    }
    {
        // "summary" only in Compact context's description.
        auto r = filtered_commands("summary");
        check(has_id(r, Command::CompactContext),
              "\"summary\" finds Compact context via description");
    }

    // ── 5. SHORTCUT match ─────────────────────────────────────────────────
    {
        auto r = filtered_commands("ctrl+g");
        check(has_id(r, Command::RunCodeBlock),
              "\"ctrl+g\" finds Run code block via its shortcut");
    }

    // ── 6. case-insensitive ───────────────────────────────────────────────
    {
        auto r = filtered_commands("QUIT");
        check(has_id(r, Command::Quit), "filter is case-insensitive");
    }

    // ── 7. label hits rank ABOVE description-only hits ────────────────────
    {
        // "changes" is in the LABELS of AcceptAll/RejectAll/ReviewChanges and
        // also in the DESCRIPTION of RewindCheckpoint ("...conversation...").
        // Whatever matches by label must precede any description-only match.
        auto r = filtered_commands("change");
        // Find the first description-only match and the last label match;
        // assert no label match comes after a description-only one.
        bool seen_desc_only = false, ok = true;
        for (const auto* c : r) {
            std::string_view lab{c->label};
            bool label_hit = false;
            std::string low;
            for (char ch : lab) low.push_back(
                static_cast<char>(std::tolower((unsigned char)ch)));
            label_hit = low.find("change") != std::string_view::npos;
            if (!label_hit) seen_desc_only = true;
            else if (seen_desc_only) ok = false;   // label hit after a desc-only
        }
        check(ok, "label matches rank above description-only matches");
    }

    // ── 8. no match → empty ───────────────────────────────────────────────
    check(filtered_commands("zznotacommandzz").empty(),
          "a non-matching query returns no rows");
}
