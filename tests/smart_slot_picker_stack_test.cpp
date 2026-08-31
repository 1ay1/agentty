// smart_slot_picker_stack_test — picker navigation is a STACK, not a trapdoor.
//
// Descending Smart Mode → model picker to assign a role slot and then
// backing out (Esc) or committing (Enter) must POP one level — return to
// the Smart Mode picker — never nuke every overlay back to the thread.
// Navigating into a sub-setting and hitting Esc should land you on the row
// you came from, so you can keep configuring the sibling slots. This guards
// the reducer paths in src/runtime/app/update/picker.cpp (ModelPickerSelect
// and CloseModelPicker, slot-assign branches).
//
// Driven through the REAL app::update reducer, no mocks of the reducer path.

#include "agtest.hpp"

#include "agentty/runtime/app/update.hpp"
#include "agentty/runtime/app/deps.hpp"
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

// A Model with a small catalog and the model picker already opened in
// slot-assign mode for `slot` (0 strategic / 1 implementation / 2 utility),
// the cursor sitting on the first catalog entry. This mirrors the state right
// after SmartModeSelect on a slot row descends into the model chooser.
Model in_slot_assign(int slot) {
    Model m;
    m.d.available_models = { mi("claude-opus-4-5", "anthropic"),
                             mi("claude-haiku-4-5", "anthropic") };
    m.ui.smart_assign_slot = slot;
    m.ui.overlay = ov::ModelPicker{{0}};
    return m;
}

} // namespace

TEST_CASE("smart slot picker stack") {
    install_stub_deps();
    {
        provider::Selection sel;
        sel.kind = provider::Kind::Anthropic;
        provider::select(sel);
    }

    // ── Esc in slot-assign pops back to Smart Mode, does NOT exit ──
    for (int slot = 0; slot <= 2; ++slot) {
        Model m = in_slot_assign(slot);
        auto [m2, cmd] = app::update(std::move(m), Msg{CloseModelPicker{}});

        CHECK(!m2.ui.overlay.is<ov::ModelPicker>(),
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
        auto [m2, cmd] = app::update(std::move(m), Msg{ModelPickerSelect{}});

        CHECK(!m2.ui.overlay.is<ov::ModelPicker>(),
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
    }

    // ── A NON-slot model-picker Esc still exits cleanly (no regression) ──
    {
        Model m;
        m.d.available_models = { mi("claude-opus-4-5", "anthropic") };
        m.ui.smart_assign_slot = -1;          // ordinary model switch
        m.ui.overlay = ov::ModelPicker{{0}};
        auto [m2, cmd] = app::update(std::move(m), Msg{CloseModelPicker{}});
        CHECK(!m2.ui.overlay.is<ov::ModelPicker>(), "ordinary Esc closes picker");
        CHECK(!m2.ui.overlay.is<ov::SmartMode>(),
              "ordinary model-switch Esc does NOT spuriously open Smart Mode");
    }
}
