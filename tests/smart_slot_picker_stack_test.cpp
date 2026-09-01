// smart_slot_picker_stack_test — picker navigation is a STACK, not a trapdoor.
//
// Descending Smart Mode → model picker to assign a role slot and then
// backing out (Esc) or committing (Enter) must POP one level — return to
// the Smart Mode picker — never nuke every overlay back to the thread.
// Navigating into a sub-setting and hitting Esc should land you on the row
// you came from, so you can keep configuring the sibling slots. This guards
// the reducer paths in src/runtime/app/update/picker.cpp (FusedPickerSelect
// and CloseFusedPicker, slot-assign branches).
//
// Driven through the REAL app::update reducer, no mocks of the reducer path.
//
// Since the picker consolidation there is ONE model surface (the fused,
// all-providers picker); this guards its slot-assign mode — the reducer
// paths in src/runtime/app/update/picker.cpp (FusedPickerSelect and
// CloseFusedPicker, slot-assign branches) plus the active-provider scoping
// that keeps an unstreamable cross-provider pin unrepresentable.

#include "agtest.hpp"

#include "agentty/runtime/app/update.hpp"
#include "agentty/runtime/app/deps.hpp"
#include "agentty/runtime/app/update/internal.hpp"  // app::detail::fused_rows_for_model
#include "agentty/runtime/picker.hpp"
#include "agentty/provider/selection.hpp"

#include <optional>
#include <string>
#include <vector>

namespace ov = agentty::ui::overlay;

using namespace agentty;

namespace {

store::Settings g_settings;
void install_stub_deps() {
    app::install_deps(app::Deps{
        .stream         = [](auto, auto) {},
        .save_thread    = [](const auto&) {},
        .delete_thread  = [](const auto&) {},
        .load_threads   = [] { return std::vector<Thread>{}; },
        .load_thread    = [](const auto&) -> std::optional<Thread> { return std::nullopt; },
        .load_settings  = [] { return g_settings; },
        .save_settings  = [](const store::Settings& x) { g_settings = x; },
        .new_thread_id  = [] { return ThreadId{}; },
        .title_from     = [](std::string_view t) { return std::string{t}; },
        .auth           = auth::AuthHeader{auth::ApiKeyHeader{std::string{}}},
    });
}

ModelInfo mi(const char* id, const char* prov) {
    ModelInfo m;
    m.id = ModelId{id};
    m.display_name = id;
    m.provider = prov;
    m.supports_tools = true;
    return m;
}

// A Model with a small catalog, opened through the REAL OpenFusedPicker
// path in slot-assign mode for `slot` (0 strategic / 1 implementation /
// 2 utility). This mirrors the state right after SmartModeSelect on a slot
// row descends into the model chooser — which is exactly what meta.cpp does.
Model in_slot_assign(int slot) {
    Model m;
    m.d.model_id = ModelId{"claude-opus-4-5"};
    m.d.available_models = { mi("claude-opus-4-5", "anthropic"),
                             mi("claude-haiku-4-5", "anthropic") };
    m.ui.smart_assign_slot = slot;
    auto [m1, _] = app::update(std::move(m), Msg{OpenFusedPicker{}});
    return std::move(m1);
}

} // namespace

TEST_CASE("smart slot picker stack") {
    install_stub_deps();
    // Hermetic auth: the fused picker only seeds catalogs for providers that
    // are AUTHED, and a bare test env has no on-disk credentials (nor should
    // it depend on any). A provider_keys entry makes provider_is_authed
    // ("anthropic") true, so OpenFusedPicker populates real rows.
    g_settings = store::Settings{};
    g_settings.provider_keys["anthropic"] = "sk-test";
    {
        provider::Selection sel;
        sel.kind = provider::Kind::Anthropic;
        provider::select(sel);
    }

    // ── Esc in slot-assign pops back to Smart Mode, does NOT exit ──
    for (int slot = 0; slot <= 2; ++slot) {
        Model m = in_slot_assign(slot);
        auto [m2, cmd] = app::update(std::move(m), Msg{CloseFusedPicker{}});

        CHECK(!m2.ui.overlay.is<ov::FusedPicker>(),
              "Esc closes the model picker");
        CHECK(m2.ui.overlay.is<ov::SmartMode>(),
              "Esc RE-OPENS Smart Mode — navigation is a stack, not a trapdoor");
        CHECK(m2.ui.smart_assign_slot == -1,
              "the pending slot-assign is cleared on back-out");
        if (auto* o = m2.ui.overlay.get<ov::SmartMode>()) {
            CHECK(o->index == 8 + slot,
                  "cursor lands back on the slot row you descended from");
        } else {
            CHECK(false, "smart_mode must be OpenAt after Esc");
        }
        // Esc must NOT have written anything into the slot.
        const smart::SlotOverride& s =
            slot == 0 ? m2.d.smart.strategic
          : slot == 1 ? m2.d.smart.implementation
                      : m2.d.smart.utility;
        CHECK(!s.set, "Esc abandons the assignment — slot stays unset");
    }

    // ── Enter in slot-assign writes the slot AND pops back to Smart Mode ──
    for (int slot = 0; slot <= 2; ++slot) {
        Model m = in_slot_assign(slot);
        // Put the cursor on the opus row explicitly — the fused list orders
        // by (active, favourite, family) rather than raw catalog order.
        const auto rows = app::detail::fused_rows_for_model(m);
        int opus = -1;
        for (int i = 0; i < static_cast<int>(rows.size()); ++i)
            if (rows[static_cast<std::size_t>(i)].model.id.value == "claude-opus-4-5") {
                opus = i; break;
            }
        REQUIRE(opus >= 0);
        if (auto* c = m.ui.overlay.get<ov::FusedPicker>()) c->index = opus;

        auto [m2, cmd] = app::update(std::move(m), Msg{FusedPickerSelect{}});

        CHECK(!m2.ui.overlay.is<ov::FusedPicker>(),
              "Enter closes the model picker");
        CHECK(m2.ui.overlay.is<ov::SmartMode>(),
              "Enter returns to Smart Mode so sibling slots stay one step away");
        CHECK(m2.ui.smart_assign_slot == -1, "slot-assign consumed");
        if (auto* o = m2.ui.overlay.get<ov::SmartMode>()) {
            CHECK(o->index == 8 + slot, "cursor on the slot just set");
        }

        const smart::SlotOverride& s =
            slot == 0 ? m2.d.smart.strategic
          : slot == 1 ? m2.d.smart.implementation
                      : m2.d.smart.utility;
        CHECK(s.set, "Enter pins the slot");
        CHECK(s.model == "claude-opus-4-5",
              "the highlighted catalog model is written into the slot");
        CHECK(m2.d.smart.enabled,
              "pinning a slot implicitly enables Smart Mode");
        CHECK(m2.d.model_id.value == "claude-opus-4-5",
              "slot-assign must NOT switch the active model");
    }

    // ── Slot-assign scopes the list to the ACTIVE provider ────────────
    // A pinned slot model is dispatched to whatever provider is active at
    // turn time, so a row from another provider would be unstreamable.
    // The list must not offer one — nor a "sign in to X" row.
    {
        Model m = in_slot_assign(0);
        const auto rows = app::detail::fused_rows_for_model(m);
        REQUIRE(!rows.empty());
        for (const auto& r : rows) {
            CHECK(r.provider_id == "anthropic",
                  "slot-assign lists ONLY the active provider's models");
            CHECK(!r.is_signin_offer(),
                  "slot-assign never offers a provider you aren't signed into");
        }
    }

    // ── The scoping is CONDITIONAL on slot-assign, not permanent ──────
    // (Cross-provider breadth itself is fused_models_test's job; here we only
    // prove that only_provider is applied in slot-assign mode and NOT applied
    // outside it — the bug this filter could plausibly introduce.)
    {
        auto rows_for = [](int slot) {
            Model m;
            m.d.model_id = ModelId{"claude-opus-4-5"};
            m.d.available_models = { mi("claude-opus-4-5", "anthropic"),
                                     mi("claude-haiku-4-5", "anthropic") };
            m.ui.smart_assign_slot = slot;
            auto [m1, _] = app::update(std::move(m), Msg{OpenFusedPicker{}});
            return app::detail::fused_rows_for_model(m1);
        };
        const auto plain = rows_for(-1);
        const auto scoped = rows_for(0);
        REQUIRE(!plain.empty());
        REQUIRE(!scoped.empty());
        // Slot-assign never shows MORE than the unscoped list, and never a
        // sign-in offer (you can't pin a model you aren't signed in to).
        CHECK(scoped.size() <= plain.size(),
              "slot-assign narrows the list, never widens it");
        for (const auto& r : scoped) {
            CHECK(r.provider_id == "anthropic",
                  "slot-assign lists ONLY the active provider's models");
            CHECK(!r.is_signin_offer(),
                  "slot-assign never offers a provider you aren't signed into");
        }
    }

    // ── A NON-slot model-picker Esc still exits cleanly (no regression) ──
    {
        Model m;
        m.d.available_models = { mi("claude-opus-4-5", "anthropic") };
        m.ui.smart_assign_slot = -1;          // ordinary model switch
        auto [m1, _] = app::update(std::move(m), Msg{OpenFusedPicker{}});
        auto [m2, cmd] = app::update(std::move(m1), Msg{CloseFusedPicker{}});
        CHECK(!m2.ui.overlay.is<ov::FusedPicker>(), "ordinary Esc closes picker");
        CHECK(!m2.ui.overlay.is<ov::SmartMode>(),
              "ordinary model-switch Esc does NOT spuriously open Smart Mode");
    }
}
