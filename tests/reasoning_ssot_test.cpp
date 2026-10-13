// reasoning_ssot_test — "reasoning ‹off›" in the picker means off on the wire.
//
// ── THE BUG ──────────────────────────────────────────────────────────────
//
// The model picker's effort strip read `reasoning ‹off›` while every single
// Copilot request went out asking for reasoning:
//
//     responses.request: … model=gpt-5.4 tools=45 reasoning=1 bytes=99174
//
// and the user got thinking blocks they had switched off, and paid for the
// tokens. Three separate inputs could each answer "do we want reasoning",
// and they disagreed:
//
//   1. the effort strip (m.d.effort)  — the live, per-model control, and
//                                       the only one bound to a key
//   2. m.d.show_reasoning             — the old ^R toggle. Its reducer arm
//                                       still existed but NOTHING dispatched
//                                       it (^R is review), so the flag was
//                                       frozen at whatever settings.json
//                                       happened to hold — `true` here.
//   3. ui.thinking != Hidden          — Appearance's DISPLAY preference
//
// On top of that, build_body sent `reasoning: {summary: "auto"}` when no
// effort tier was set. That is not "no opinion" — it is an explicit request
// for a reasoning summary. So even with every toggle off, the wire asked.
//
// ── THE RULE ─────────────────────────────────────────────────────────────
//
//     The effort strip is the SSOT. No tier → no reasoning on the wire.
//
// "How hard should this model think" is a property of the model you are
// picking. "Do I want to look at it" is a property of your UI, and belongs
// to Appearance — but a DISPLAY choice must never silently change what we
// REQUEST, because that is how tokens get paid for and dropped.

#include <string>

#include <nlohmann/json.hpp>

#include "agtest.hpp"

#include "agentty/provider/chatgpt/responses.hpp"

using namespace agentty;
namespace cc = agentty::provider::chatgpt;
using json = nlohmann::json;

namespace {

// A minimal well-formed request; the effort tier is what each case varies.
provider::Request req_with_effort(std::string effort) {
    provider::Request req;
    req.model         = "gpt-5.4";
    req.system_prompt = "You are a coding agent.";
    req.effort        = std::move(effort);

    Message u;
    u.role = Role::User;
    u.text = "list the files";
    req.messages.push_back(u);
    return req;
}

}  // namespace

TEST_CASE("reasoning ssot: no effort tier sends no reasoning field") {
    // The exact configuration from the bug report: the strip says off, so
    // nothing about reasoning may appear on the wire. Sending
    // `{"summary":"auto"}` here is what produced thinking blocks on a turn
    // the user had switched off.
    const json body = cc::build_body_for_test(req_with_effort(""));

    CHECK(!body.contains("reasoning"));
}

TEST_CASE("reasoning ssot: an effort tier sends that tier plus a summary") {
    // The other half of the contract — `off` meaning off is only useful if
    // a real tier still asks for the summary text. Without `summary` a
    // reasoning model burns thinking tokens and returns no visible trace,
    // which is the same waste in the opposite direction.
    for (const char* tier : {"minimal", "low", "medium", "high", "xhigh"}) {
        const json body = cc::build_body_for_test(req_with_effort(tier));

        REQUIRE(body.contains("reasoning"));
        CHECK(body["reasoning"]["effort"] == tier);
        CHECK(body["reasoning"]["summary"] == "auto");
    }
}

TEST_CASE("reasoning ssot: the field is present iff a tier is set") {
    // State the invariant as one biconditional, so a future edit that adds
    // a third arm ("omit the tier but keep the summary", the exact shape of
    // the original bug) fails here rather than on someone's bill.
    for (const char* tier : {"", "minimal", "low", "medium", "high", "xhigh"}) {
        const std::string effort{tier};
        const json body = cc::build_body_for_test(req_with_effort(effort));

        CHECK(body.contains("reasoning") == !effort.empty());
    }
}
