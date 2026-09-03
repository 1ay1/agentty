// visual_hash_coverage_test — proves the render gate can't silently
// drop a visible change.
//
// THE BUG CLASS THIS GUARDS
// =========================
// maya's run loop skips view()+render() whenever
// `Program::visual_hash(model)` matches the previous frame's value
// (app.hpp). That gate is what lets a 30 fps Tick fire cheaply: a tick
// that changed nothing visible produces the same hash and costs zero
// layout/paint. The hazard is the inverse — if a model field DOES
// affect the rendered frame but is NOT mixed into visual_hash, a
// mutation to it produces an identical hash, the gate fires, and the
// change never paints until some UNRELATED hashed axis happens to flip.
// The failure is SILENT: no crash, no log, just a region of the UI that
// stops updating until the next keystroke / spinner tick. This is
// exactly the production "picker arrow keys register once per 4-5
// presses" regression (CHANGELOG): picker selection index wasn't in the
// hash, so cursor moves were gated away.
//
// THE CONTRACT
// ============
// Every field that affects the rendered view MUST advance visual_hash
// when it changes. Conversely, fields that DON'T affect pixels (the
// tick clock, token counters, cancel handles) MUST NOT advance it —
// otherwise the gate is defeated and every tick repaints, reintroducing
// the per-frame layout cost the gate exists to avoid.
//
// HOW THIS TEST ENFORCES IT
// =========================
// Two declarative tables:
//   • kVisualAxes    — {name, mutate}. For each: snapshot the hash,
//                      apply a visually-meaningful mutation, assert the
//                      hash CHANGED. A new view-affecting field that the
//                      author forgot to mix into visual_hash fails here
//                      the moment they add it to this table — and the
//                      table is the natural place to add it, so the
//                      omission is caught at the same edit.
//   • kInvariantAxes — {name, mutate}. For each: assert the hash did
//                      NOT change. Guards against over-hashing (mixing
//                      last_tick would make every tick repaint).
//
// The tables ARE the spec. When you add a model field that the view
// reads, add a row to kVisualAxes; the test then requires the matching
// `mix()` line in program.hpp. When you add ephemeral state the view
// ignores, add a row to kInvariantAxes.

#include <chrono>
#include <functional>
#include <string>
#include <vector>

#include "agtest.hpp"

#include "agentty/runtime/app/program.hpp"
#include "agentty/runtime/model.hpp"

namespace ov = agentty::ui::overlay;
namespace smart = agentty::smart;

using agentty::Model;
using agentty::app::AgenttyApp;

namespace {

int g_checks = 0;


std::uint64_t hash_of(const Model& m) { return AgenttyApp::visual_hash(m); }

// A baseline model that is "settled and idle" — no active stream, empty
// composer, no modal open, a couple of messages so frozen/live axes are
// meaningful. Each axis test starts from a fresh copy of this so axes
// don't interfere.
Model baseline() {
    Model m;
    // Two settled messages so messages.size() / render_key axes have
    // something to move against.
    agentty::Message u;
    u.role = agentty::Role::User;
    u.text = "hello";
    m.d.current.messages.push_back(u);
    agentty::Message a;
    a.role = agentty::Role::Assistant;
    a.text = "hi there";
    m.d.current.messages.push_back(a);
    m.s.phase = agentty::phase::Idle{};
    return m;
}

// ── A mutation that changes one axis in a visually-meaningful way. ──
struct Axis {
    const char*                 name;
    std::function<void(Model&)> mutate;
};

// Fields that AFFECT the rendered frame. Each mutation must move the
// hash. Mirror the axes program.hpp::visual_hash mixes; a gap between
// these tables and that function is the bug this test exists to catch.
const std::vector<Axis>& visual_axes() {
    static const std::vector<Axis> axes = {
        {"messages.size (append a turn)", [](Model& m) {
            agentty::Message x; x.role = agentty::Role::User; x.text = "new";
            m.d.current.messages.push_back(x);
        }},
        {"live tail message text (render_key)", [](Model& m) {
            m.d.current.messages.back().text += " more";
        }},
        {"live tail pending_stream (render_key)", [](Model& m) {
            // A delta that lands only in the Tick pacer's buffer must
            // still advance the hash, or the render gate skips the frame
            // and the live tail's reveal stops re-arming — the stream
            // freezes until an unrelated axis flips.
            m.d.current.messages.back().pending_stream += "buffered";
        }},
        {"profile cycle", [](Model& m) {
            m.d.profile = (m.d.profile == agentty::Profile::Write)
                        ? agentty::Profile::Ask : agentty::Profile::Write;
        }},
        {"reasoning effort tier", [](Model& m) {
            // The effort tier renders in the model badge (and picker line);
            // ←/→ must repaint. High → Low is a visible change.
            m.d.effort = (m.d.effort == agentty::Effort::High)
                       ? agentty::Effort::Low : agentty::Effort::High;
        }},
        {"model_id swap", [](Model& m) {
            m.d.model_id = agentty::ModelId{std::string{"claude-haiku-4-5"}};
        }},
        {"pending_permission appears", [](Model& m) {
            m.d.pending_permission = agentty::PendingPermission{};
        }},
        {"phase Idle -> Streaming", [](Model& m) {
            m.s.phase = agentty::phase::Streaming{agentty::phase::Active{}};
        }},
        {"status banner text", [](Model& m) {
            m.s.status = "something happened";
        }},
        {"status expiry (100ms bucket)", [](Model& m) {
            m.s.status = "x";
            m.s.status_until = std::chrono::steady_clock::now()
                             + std::chrono::seconds(5);
        }},
        {"spinner frame (while active)", [](Model& m) {
            m.s.phase = agentty::phase::Streaming{agentty::phase::Active{}};
            // Advance the spinner past a full frame-interval so
            // frame_index() changes.
            m.s.spinner.advance(1.0f);
        }},
        {"composer text", [](Model& m) {
            m.ui.composer.text = "typing";
        }},
        {"composer cursor", [](Model& m) {
            m.ui.composer.text = "abc";
            m.ui.composer.cursor = 2;
        }},
        {"composer attachment count", [](Model& m) {
            m.ui.composer.attachments.push_back(agentty::Attachment{});
        }},
        {"composer queued count", [](Model& m) {
            m.ui.composer.queued.push_back({"queued msg", {}});
        }},
        {"composer expanded toggle", [](Model& m) {
            m.ui.composer.expanded = true;
        }},
        {"frozen prefix grows", [](Model& m) {
            m.ui.frozen.seal(maya::Element{}, 1);
        }},
        {"frozen_turn advances", [](Model& m) {
            m.ui.frozen_turn = 7;
        }},
        {"model_picker opens", [](Model& m) {
            m.ui.overlay = ov::FusedPicker{{0, ""}};
        }},
        {"model_picker cursor move", [](Model& m) {
            m.ui.overlay = ov::FusedPicker{{3}};
        }},
        {"model_picker query", [](Model& m) {
            agentty::ui::pick::OpenAt o; o.index = 0; o.query = "free";
            m.ui.overlay = ov::FusedPicker{std::move(o)};
        }},
        {"provider_picker opens", [](Model& m) {
            m.ui.overlay = ov::ProviderPicker{{0}};
        }},
        {"provider_picker cursor move", [](Model& m) {
            m.ui.overlay = ov::ProviderPicker{{2}};
        }},
        {"thread_list opens", [](Model& m) {
            m.ui.overlay = ov::ThreadList{{0}};
        }},
        {"thread_list cursor move", [](Model& m) {
            m.ui.overlay = ov::ThreadList{{4}};
        }},
        {"thread_list delete confirm", [](Model& m) {
            auto o = agentty::ui::pick::OpenAt{2};
            o.confirm_remove = "abc123";
            m.ui.overlay = agentty::ui::overlay::ThreadList{std::move(o)};
        }},
        {"diff_review opens at cell", [](Model& m) {
            m.ui.overlay = ov::DiffReview{{0, 0}};
        }},
        {"diff_review hunk move", [](Model& m) {
            m.ui.overlay = ov::DiffReview{{1, 2}};
        }},
        {"command_palette opens", [](Model& m) {
            m.ui.overlay = ov::CommandPalette{{}};
        }},
        {"command_palette query", [](Model& m) {
            m.ui.overlay = ov::CommandPalette{{"git", 0}};
        }},
        {"command_palette index", [](Model& m) {
            m.ui.overlay = ov::CommandPalette{{"git", 5}};
        }},
        {"mention_palette opens", [](Model& m) {
            m.ui.overlay = agentty::ui::overlay::Mention{};
        }},
        {"mention_palette query", [](Model& m) {
            agentty::mention::Open o; o.query = "src"; o.index = 0;
            m.ui.overlay = ov::Mention{std::move(o)};
        }},
        {"mention_palette index", [](Model& m) {
            agentty::mention::Open o; o.query = "src"; o.index = 3;
            m.ui.overlay = ov::Mention{std::move(o)};
        }},
        {"symbol_palette opens", [](Model& m) {
            m.ui.overlay = agentty::ui::overlay::Symbol{};
        }},
        {"symbol_palette query", [](Model& m) {
            agentty::symbol_palette::Open o; o.query = "foo"; o.index = 0;
            m.ui.overlay = ov::Symbol{std::move(o)};
        }},
        {"symbol_palette index", [](Model& m) {
            agentty::symbol_palette::Open o; o.query = "foo"; o.index = 2;
            m.ui.overlay = ov::Symbol{std::move(o)};
        }},
        {"todo modal opens", [](Model& m) {
            m.ui.todo.open = agentty::ui::pick::OpenModal{};
        }},
        {"tool viewer opens", [](Model& m) {
            m.ui.overlay = ov::ToolViewer{{{}, 0, false}};
        }},
        {"tool viewer list cursor move", [](Model& m) {
            m.ui.overlay = ov::ToolViewer{{{}, 2, false}};
        }},
        {"tool viewer list -> body stage", [](Model& m) {
            m.ui.overlay = ov::ToolViewer{{{}, 0, true}};
        }},
        {"tool viewer body scroll", [](Model& m) {
            m.ui.overlay = ov::ToolViewer{{{}, 0, true}};
            m.ui.tool_viewer_scroll.y = 5;
        }},
        {"tool viewer live tail toggled", [](Model& m) {
            m.ui.overlay = ov::ToolViewer{{
                {agentty::tool_viewer::Entry{}}, 0, /*viewing=*/true}};
            m.ui.tool_viewer_tail = false;
        }},
        {"code block picker opens", [](Model& m) {
            m.ui.overlay = ov::CodeBlocks{{{}, 0}};
        }},
        {"code block picker cursor move", [](Model& m) {
            m.ui.overlay = ov::CodeBlocks{{{}, 3}};
        }},
        {"login modal opens", [](Model& m) {
            m.ui.login = agentty::ui::login::Picking{};
        }},
        {"login chatgpt waiting", [](Model& m) {
            m.ui.login = agentty::ui::login::ChatGptWaiting{};
        }},
        {"settings_list opens (Plugins)", [](Model& m) {
            agentty::settings::ListOpen o;
            o.concern = agentty::settings::Category::Plugins;
            m.ui.overlay = agentty::ui::overlay::SettingsList{o};
        }},
        {"settings_list cursor move", [](Model& m) {
            agentty::settings::ListOpen o;
            o.concern = agentty::settings::Category::Plugins;
            o.index = 3;
            m.ui.overlay = agentty::ui::overlay::SettingsList{o};
        }},
        {"settings_list add-mode input", [](Model& m) {
            agentty::settings::ListOpen o;
            o.concern = agentty::settings::Category::Plugins;
            o.input_active = true;
            o.input = "date -- /path/date_server";
            o.cursor = 4;
            m.ui.overlay = agentty::ui::overlay::SettingsList{o};
        }},
        {"plugins loading flag", [](Model& m) {
            m.ui.plugins_loading = true;
        }},
        {"plugins snapshot lands (server connected)", [](Model& m) {
            agentty::mcp::ServerState s;
            s.name = "date";
            s.connected = true;
            s.tools.push_back({"current_date", "", true, false});
            m.ui.plugins.servers.push_back(std::move(s));
        }},
        {"plugins server disabled toggles the hash", [](Model& m) {
            agentty::mcp::ServerState s;
            s.name = "date";
            s.disabled = true;   // distinct from plain disconnected
            s.tools.push_back({"current_date", "", true, false});
            m.ui.plugins.servers.push_back(std::move(s));
        }},
        {"smart_mode overlay opens", [](Model& m) {
            m.ui.overlay = ov::SmartMode{smart::OverlayRow::Master};
        }},
        {"smart_mode cursor move", [](Model& m) {
            m.ui.overlay = ov::SmartMode{smart::OverlayRow::Utility};
        }},
        {"rag picker opens", [](Model& m) {
            agentty::rag_settings::Open o;
            m.ui.overlay = agentty::ui::overlay::RagSettings{o};
        }},
        {"rag picker cursor move", [](Model& m) {
            agentty::rag_settings::Open o;
            o.cursor = agentty::store::RagMode::Off;
            m.ui.overlay = agentty::ui::overlay::RagSettings{o};
        }},
        {"fork picker opens", [](Model& m) {
            m.ui.overlay = ov::Fork{{agentty::fork_picker::Choice::RagPerTurn}};
        }},
        {"fork picker cursor move", [](Model& m) {
            m.ui.overlay = ov::Fork{{agentty::fork_picker::Choice::RagOff}};
        }},
    };
    return axes;
}

// Fields the view does NOT read. Mutating these MUST NOT move the hash,
// or the render gate is defeated (every tick repaints). These are the
// dual of the contract: visual_hash's whole value is in what it OMITS.
const std::vector<Axis>& invariant_axes() {
    static const std::vector<Axis> axes = {
        {"last_tick clock", [](Model& m) {
            m.s.last_tick = std::chrono::steady_clock::now()
                          + std::chrono::hours(1);
        }},
        {"token counters", [](Model& m) {
            m.s.tokens_in  = 12345;
            m.s.tokens_out = 67890;
        }},
        {"pending rehydrate trim", [](Model& m) {
            m.ui.pending_rehydrate_trim = true;
        }},
    };
    return axes;
}

}  // namespace (helpers)

TEST_CASE("visual axes advance hash") {
    std::printf("visual_hash: each view axis advances the hash\n");
    for (const auto& ax : visual_axes()) {
        Model before = baseline();
        const std::uint64_t h0 = hash_of(before);
        Model after = baseline();
        ax.mutate(after);
        const std::uint64_t h1 = hash_of(after);
        check(h0 != h1,
              std::string("axis '") + ax.name +
              "' did NOT change visual_hash — the view reads it but "
              "program.hpp::visual_hash forgot to mix() it. Renders for "
              "this change will be gated away (silent dead region).");
    }
}

TEST_CASE("invariant axes preserve hash") {
    std::printf("visual_hash: non-visual axes do NOT advance the hash\n");
    for (const auto& ax : invariant_axes()) {
        Model before = baseline();
        const std::uint64_t h0 = hash_of(before);
        Model after = baseline();
        ax.mutate(after);
        const std::uint64_t h1 = hash_of(after);
        check(h0 == h1,
              std::string("axis '") + ax.name +
              "' CHANGED visual_hash but the view ignores it — every "
              "Tick will now defeat the render gate and repaint. Remove "
              "its mix() from program.hpp::visual_hash.");
    }
}

// Sanity: the baseline itself is stable across two calls (no time term
// leaking for a settled/idle model — regime (c): nothing animating).
TEST_CASE("idle settled is stable") {
    std::printf("visual_hash: settled idle model is hash-stable across calls\n");
    Model m = baseline();
    const std::uint64_t a = hash_of(m);
    // No mutation, no sleep — an idle settled model (no active stream,
    // a non-empty thread so the welcome bob is off, empty queue) must
    // contribute no time term, so two reads match.
    const std::uint64_t b = hash_of(m);
    check(a == b,
          "idle settled model produced two different hashes — a time "
          "term is leaking into a regime (c) state (should be none).");
}

// ── Spinner gates: subscription, advance and hash must agree ────────────
//
// A time-based animation in this app passes through THREE independent
// gates, and all three must name the same condition:
//
//   1. subscribe.cpp   — is a Tick even delivered?
//   2. update/meta.cpp — does the spinner advance on that Tick?
//   3. program.hpp     — does the new frame change the visual hash
//                        (i.e. does anything repaint)?
//
// Disagreement is SILENT: a frame that advances without being hashed
// animates invisibly; one hashed without advancing burns renders on an
// unchanged glyph. The fused picker's "loading …" spinner needs all
// three while `models_loading` is set — a frozen glyph reads as a hang,
// which is the exact opposite of what it exists to say.
TEST_CASE("visual hash: spinner advances the hash while models load") {
    Model m;
    REQUIRE(!m.s.active());          // idle: the streaming gate is off
    m.s.models_loading = true;

    const auto h0 = agentty::app::AgenttyApp::visual_hash(m);
    m.s.spinner.advance(0.5f);       // enough for a new frame index
    const auto h1 = agentty::app::AgenttyApp::visual_hash(m);
    CHECK(h0 != h1);
}

TEST_CASE("visual hash: an idle picker with no load does NOT animate") {
    // The complement: without models_loading (and not streaming) the
    // spinner must stay out of the hash, or an idle agentty repaints
    // forever for a glyph nobody is looking at.
    Model m;
    REQUIRE(!m.s.active());
    m.s.models_loading = false;

    const auto h0 = agentty::app::AgenttyApp::visual_hash(m);
    m.s.spinner.advance(0.5f);
    CHECK(agentty::app::AgenttyApp::visual_hash(m) == h0);
}

TEST_CASE("visual hash: spinner animates while a PICKER catalog loads") {
    // The picker fans out to every authed provider and tracks each fetch on
    // ProviderCatalog::state — it never sets Session::models_loading (that
    // covers the ACTIVE provider only: provider switch / startup). Gating
    // the spinner on the wrong flag left it frozen in the one surface it
    // exists for, which reads as a hang.
    Model m;
    REQUIRE(!m.s.active());
    REQUIRE(!m.s.models_loading);
    agentty::ProviderCatalog c;
    c.provider_id = "openai";
    c.state = agentty::ProviderCatalog::State::Loading;
    m.d.provider_catalogs.push_back(std::move(c));
    m.ui.overlay = agentty::ui::overlay::FusedPicker{};
    REQUIRE(m.loading_spinner_visible());

    const auto h0 = agentty::app::AgenttyApp::visual_hash(m);
    m.s.spinner.advance(0.5f);
    CHECK(agentty::app::AgenttyApp::visual_hash(m) != h0);
}

TEST_CASE("visual hash: a READY catalog does not animate") {
    Model m;
    agentty::ProviderCatalog c;
    c.provider_id = "openai";
    c.state = agentty::ProviderCatalog::State::Ready;
    m.d.provider_catalogs.push_back(std::move(c));
    m.ui.overlay = agentty::ui::overlay::FusedPicker{};
    REQUIRE(!m.loading_spinner_visible());

    const auto h0 = agentty::app::AgenttyApp::visual_hash(m);
    m.s.spinner.advance(0.5f);
    CHECK(agentty::app::AgenttyApp::visual_hash(m) == h0);
}
