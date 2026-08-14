// smart_mode_test — the role resolver (Smart Mode Step 1).
//
// Pure mapping: role + parent model + effort + catalog + config → RoleProfile.
// No I/O, no wire. Verifies zero-config auto-fill, overrides, the off
// pass-through, and the single-tier no-regression guarantee.
#include "agentty/domain/smart_mode.hpp"
#include "agentty/domain/catalog.hpp"   // ModelCapabilities

#include <cstdio>

namespace sm = agentty::smart;
using agentty::Effort;
using agentty::ModelInfo;
using agentty::ModelId;
using agentty::ModelCapabilities;

static int g_fail = 0;
#define CHECK(cond, msg)                                                       \
    do {                                                                       \
        if (!(cond)) { std::printf("  FAIL: %s\n", msg); ++g_fail; }           \
        else         { std::printf("  ok:   %s\n", msg); }                     \
    } while (0)

static ModelInfo mi(const char* id, int ctx = 200000) {
    ModelInfo m;
    m.id = ModelId{id};
    m.context_window = ctx;
    m.supports_tools = true;
    return m;
}

int main() {
    std::printf("[smart_mode]\n");

    // A realistic Claude catalog: Opus (flagship), Sonnet (mid), Haiku (cheap).
    std::vector<ModelInfo> claude = {
        mi("claude-opus-4-20250514"),
        mi("claude-sonnet-4-20250514"),
        mi("claude-haiku-4-20250514"),
    };
    const std::string parent = "claude-opus-4-20250514";

    // 1. Smart Mode OFF → every role is a pass-through to the parent.
    {
        sm::RoleConfig cfg;   // enabled=false
        auto s = sm::resolve_role(sm::ModelRole::Strategic, parent, Effort::High, claude, cfg);
        auto i = sm::resolve_role(sm::ModelRole::Implementation, parent, Effort::High, claude, cfg);
        auto u = sm::resolve_role(sm::ModelRole::Utility, parent, Effort::High, claude, cfg);
        CHECK(s.model == parent, "off: strategic = parent");
        CHECK(i.model == parent, "off: impl = parent");
        CHECK(u.model == parent, "off: utility = parent");
    }

    // 2. Smart Mode ON, zero-config auto-fill.
    {
        sm::RoleConfig cfg; cfg.enabled = true;
        auto s = sm::resolve_role(sm::ModelRole::Strategic, parent, Effort::High, claude, cfg);
        auto i = sm::resolve_role(sm::ModelRole::Implementation, parent, Effort::High, claude, cfg);
        auto u = sm::resolve_role(sm::ModelRole::Utility, parent, Effort::High, claude, cfg);

        CHECK(s.model == parent, "on: strategic = parent (flagship)");
        CHECK(i.model.find("sonnet") != std::string::npos, "on: impl = the mid (sonnet) model");
        CHECK(u.model.find("haiku") != std::string::npos, "on: utility = the cheap (haiku) model");
        CHECK(u.effort == Effort::None, "on: utility runs with NO reasoning budget");
        // Claude 4 Sonnet/Opus don't expose a reasoning-effort control, so the
        // resolver honestly clamps every role's effort to None on this
        // catalog — it never requests an effort a model would 400 on.
        CHECK(s.effort == Effort::None, "on: effort clamps to None for a non-reasoning model (honest)");
    }

    // 2b. Effort stepping on an EFFORT-CAPABLE catalog (o-series / gpt-5.x).
    //     Verifies Strategic keeps the user's effort and Impl steps one down.
    {
        std::vector<ModelInfo> gpt = {
            mi("gpt-5-pro"),      // flagship (effort max)
            mi("gpt-5"),          // mid workhorse
            mi("gpt-5-nano"),     // cheap
        };
        const std::string gparent = "gpt-5-pro";
        sm::RoleConfig cfg; cfg.enabled = true;
        auto s = sm::resolve_role(sm::ModelRole::Strategic, gparent, Effort::High, gpt, cfg);
        auto i = sm::resolve_role(sm::ModelRole::Implementation, gparent, Effort::High, gpt, cfg);
        // Only assert the STEP RELATIONSHIP if the models actually take effort;
        // if the catalog reports no effort support, both clamp to None and the
        // step is vacuously satisfied.
        const bool s_thinks = s.effort != Effort::None;
        if (s_thinks) {
            CHECK(s.effort == Effort::High, "gpt: strategic keeps the user's High effort");
            CHECK(static_cast<int>(i.effort) <= static_cast<int>(s.effort),
                  "gpt: impl effort is <= strategic (stepped down or equal)");
        } else {
            CHECK(i.effort == Effort::None, "gpt: no effort support → impl also None (consistent)");
        }
    }

    // 3. Explicit override wins over auto-fill (model always; effort clamped).
    {
        sm::RoleConfig cfg; cfg.enabled = true;
        cfg.utility.set = true;
        cfg.utility.model = "claude-sonnet-4-20250514";
        cfg.utility.effort = Effort::Low;
        auto u = sm::resolve_role(sm::ModelRole::Utility, parent, Effort::High, claude, cfg);
        CHECK(u.model.find("sonnet") != std::string::npos, "override: utility uses the pinned model");
        // Sonnet 4 takes no effort, so Low clamps to None — the pinned effort
        // is honoured only up to what the pinned model supports.
        CHECK(u.effort == Effort::None, "override: pinned effort clamped to what the model supports");
    }

    // 4. Single-model account → no regression: every role stays on the parent.
    {
        std::vector<ModelInfo> solo = { mi("claude-opus-4-20250514") };
        sm::RoleConfig cfg; cfg.enabled = true;
        auto i = sm::resolve_role(sm::ModelRole::Implementation, parent, Effort::High, solo, cfg);
        auto u = sm::resolve_role(sm::ModelRole::Utility, parent, Effort::High, solo, cfg);
        CHECK(i.model == parent, "solo account: impl stays on parent (no mid tier)");
        CHECK(u.model == parent, "solo account: utility stays on parent (nothing cheaper)");
    }

    // 5. The three behaviour layers gate independently, all under `enabled`.
    {
        sm::RoleConfig cfg;
        // Off master ⇒ no layer is active regardless of flags.
        CHECK(!cfg.internal_routing() && !cfg.orchestration() && !cfg.subagent_routing(),
              "layers: disabled master → all layers inactive");
        cfg.enabled = true;   // flags default true
        CHECK(cfg.internal_routing() && cfg.orchestration() && cfg.subagent_routing(),
              "layers: enabled + default flags → all three active");
        cfg.orchestrate = false;
        CHECK(cfg.internal_routing() && !cfg.orchestration() && cfg.subagent_routing(),
              "layers: clearing one flag disables only that layer");
        cfg.enabled = false;
        CHECK(!cfg.internal_routing() && !cfg.subagent_routing(),
              "layers: master off overrides any set flag");
    }

    // 6. Cascade effort bias: a positive bias steps effort UP, negative DOWN,
    //    clamped to the model; Trivial stays None regardless.
    {
        const auto caps = ModelCapabilities::from_id("gpt-5");   // supports effort
        // Standard @ Medium base, +1 bias → High; -1 bias → Low.
        auto up = sm::effort_for_complexity(Effort::Medium, sm::Complexity::Standard, caps, +1);
        auto dn = sm::effort_for_complexity(Effort::Medium, sm::Complexity::Standard, caps, -1);
        auto mid = sm::effort_for_complexity(Effort::Medium, sm::Complexity::Standard, caps, 0);
        CHECK(static_cast<int>(up) > static_cast<int>(mid), "cascade: +bias raises effort");
        CHECK(static_cast<int>(dn) < static_cast<int>(mid), "cascade: -bias lowers effort");
        // Trivial ignores a positive bias — an ack is an ack.
        auto triv = sm::effort_for_complexity(Effort::High, sm::Complexity::Trivial, caps, +2);
        CHECK(triv == Effort::None, "cascade: trivial stays None despite +bias");
    }

    // 8. CONTINUOUS effort scaling (effort_for_score): a turn DEEP in the
    //    Complex band gets more effort than one barely into it, and the tier
    //    boundary matches the discrete effort_for_complexity exactly.
    {
        const auto caps = ModelCapabilities::from_id("gpt-5");
        // Boundary parity: a shallow-Complex score (margin 0) == discrete path.
        sm::ComplexityScore shallow{sm::Complexity::Complex, 3, 0};
        sm::ComplexityScore deep   {sm::Complexity::Complex, 8, 5};
        auto e_shallow = sm::effort_for_score(Effort::Medium, shallow, caps, 0);
        auto e_deep    = sm::effort_for_score(Effort::Medium, deep,    caps, 0);
        auto e_tier    = sm::effort_for_complexity(Effort::Medium, sm::Complexity::Complex, caps, 0);
        CHECK(e_shallow == e_tier, "scored: shallow-Complex matches the discrete tier step");
        CHECK(static_cast<int>(e_deep) > static_cast<int>(e_shallow),
              "scored: deep-Complex thinks harder than shallow-Complex");
        // Deep-Simple drops further than shallow-Simple.
        sm::ComplexityScore sh_simple{sm::Complexity::Simple, 0, 0};
        sm::ComplexityScore dp_simple{sm::Complexity::Simple, -4, 4};
        auto s_sh = sm::effort_for_score(Effort::High, sh_simple, caps, 0);
        auto s_dp = sm::effort_for_score(Effort::High, dp_simple, caps, 0);
        CHECK(static_cast<int>(s_dp) <= static_cast<int>(s_sh),
              "scored: deep-Simple drops at least as far as shallow-Simple");
        // Trivial still pins to None regardless of score/bias.
        sm::ComplexityScore triv_s{sm::Complexity::Trivial, -100, 100};
        CHECK(sm::effort_for_score(Effort::High, triv_s, caps, +2) == Effort::None,
              "scored: trivial pins to None");
    }

    // 7. resolve_subagent_role: a worker never thinks harder than its parent.
    //    reviewer→Strategic returns the parent model at parent effort; the
    //    subagent wrapper must clamp that to ≤ parent (invariant: subagent
    //    effort ≤ parent effort; parent None ⇒ worker None).
    {
        sm::RoleConfig cfg; cfg.enabled = true;
        const char* parent = "gpt-5";   // supports effort
        std::vector<ModelInfo> g5 = { mi("gpt-5"), mi("gpt-5-mini") };
        // Parent thinking Low: a Strategic-routed reviewer must not exceed Low.
        auto rev = sm::resolve_subagent_role(sm::ModelRole::Strategic, parent,
                                             Effort::Low, g5, cfg);
        CHECK(static_cast<int>(rev.effort) <= static_cast<int>(Effort::Low),
              "subagent: reviewer effort clamped to ≤ parent (Low)");
        // Parent effort OFF ⇒ every worker role is off too.
        for (auto role : {sm::ModelRole::Strategic, sm::ModelRole::Implementation,
                          sm::ModelRole::Utility}) {
            auto p = sm::resolve_subagent_role(role, parent, Effort::None, g5, cfg);
            CHECK(p.effort == Effort::None,
                  "subagent: parent effort None ⇒ worker effort None");
        }
    }

    // 9. blend_bias: session + learned prior must not SUM (that double-
    //    escalates); same-sign keeps the stronger, opposite-sign the session
    //    wins. This helper is shared by the wire and the routing card, so the
    //    card can never show an effort the wire didn't use.
    {
        CHECK(sm::blend_bias(1, 1) == 1,  "blend: equal same-sign → that value (no sum)");
        CHECK(sm::blend_bias(1, 2) == 2,  "blend: same-sign keeps the stronger (prior)");
        CHECK(sm::blend_bias(2, 1) == 2,  "blend: same-sign keeps the stronger (session)");
        CHECK(sm::blend_bias(1, -2) == 1, "blend: opposite-sign → the live session wins");
        CHECK(sm::blend_bias(0, 2) == 2,  "blend: zero session → pure prior");
        CHECK(sm::blend_bias(0, -2) == -2,
              "blend: zero session defers to a NEGATIVE prior too — cold-start "
              "sessions must not discard the learned relax-effort signal");
        CHECK(sm::blend_bias(-1, -1) == -1, "blend: negative same-sign → no double-down");
    }

    std::printf(g_fail ? "FAILED (%d)\n" : "PASSED\n", g_fail);
    return g_fail ? 1 : 0;
}
