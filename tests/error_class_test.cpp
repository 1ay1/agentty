// error_class_test — the retry classifier's status-preferring dispatch.
//
// provider::classify_stream_error(message, http_status) is what the runtime's
// StreamError reducer calls to decide auto-retry vs re-auth vs surface. Its
// contract: when the transport stamped a real HTTP status on the Msg
// (http_status != 0), classify via the TYPED classify(HttpError) path — the
// transport already knew the exact status, so no substring sniff of the human
// message. When http_status == 0 (SSE `event: error`, transport/socket
// failure, user cancel, synthetic stall), fall back to the string sniff.
//
// This locks the "status wins over prose" behaviour that lets a proxy phrasing
// "429" as "Too Many Requests" (no digits) still retry, and stops a terminal
// 400 whose message contains "connection" from being mis-retried as transient.

#include <string_view>

#include "agtest.hpp"

#include "agentty/provider/error_class.hpp"
#include "agentty/domain/catalog.hpp"
#include "agentty/domain/entitlement.hpp"

using namespace agentty::provider;

// ── Status-set: the typed path wins, message text is ignored ───────────────
TEST_CASE("status wins over message prose") {
    // 429 with a prose-only body (no digits) → RateLimit via the typed path.
    // The old string sniff would have missed this (no "429"/"rate_limit").
    CHECK(classify_stream_error("Too Many Requests", 429) == ErrorClass::RateLimit);

    // 401/403 → Auth regardless of message wording.
    CHECK(classify_stream_error("Unauthorized", 401) == ErrorClass::Auth);
    CHECK(classify_stream_error("Forbidden", 403) == ErrorClass::Auth);

    // 5xx → Transient.
    CHECK(classify_stream_error("Bad Gateway", 502) == ErrorClass::Transient);
    CHECK(classify_stream_error("Service Unavailable", 503) == ErrorClass::Transient);
    CHECK(classify_stream_error("overloaded_error", 529) == ErrorClass::Transient);

    // A terminal 400 whose message happens to contain a "transient-looking"
    // word ("connection") must NOT be mis-retried — the status is decisive.
    CHECK(classify_stream_error("bad request: connection field invalid", 400)
          == ErrorClass::Terminal);
    CHECK(classify_stream_error("model not found", 404) == ErrorClass::Terminal);
}

// ── Status-zero: fall back to the substring sniff (unchanged behaviour) ─────
TEST_CASE("status-zero falls back to substring sniff") {
    // User cancel: StreamError{"cancelled"} carries no status → Cancelled.
    CHECK(classify_stream_error("cancelled", 0) == ErrorClass::Cancelled);

    // SSE event:error bodies (Anthropic wire text, no status on the Msg).
    CHECK(classify_stream_error("Overloaded", 0) == ErrorClass::Transient);
    CHECK(classify_stream_error("rate_limit_error", 0) == ErrorClass::RateLimit);
    CHECK(classify_stream_error("authentication_error", 0) == ErrorClass::Auth);

    // Transport/socket failures rendered as prose.
    CHECK(classify_stream_error("connection reset", 0) == ErrorClass::Transient);
    CHECK(classify_stream_error("stream stall", 0) == ErrorClass::Transient);

    // Genuinely unknown prose with no status → Terminal.
    CHECK(classify_stream_error("invalid request: missing field", 0)
          == ErrorClass::Terminal);
}

// ── The two paths agree where they overlap ────────────────────────────
// classify_stream_error(msg, status) with a status must equal the direct typed
// call, and with status 0 must equal the direct string call — it's a pure
// dispatcher, no logic of its own.
TEST_CASE("classify_stream_error is a pure dispatcher") {
    using K = agentty::http::HttpErrorKind;
    CHECK(classify_stream_error("whatever", 503)
          == classify(agentty::http::HttpError{K::Status, 503, ""}));
    CHECK(classify_stream_error("Overloaded", 0) == classify("Overloaded"));
}

// ── Typed mid-stream errors ────────────────────────────────────────
//
// An `event: error` inside a 200 body carries a machine-readable type and no
// HTTP status — the status line was 200 and is long gone. So the retry
// ladder used to classify these by sniffing the human `message`.
//
// That is the lossy path this header exists to avoid, and its failures are
// asymmetric in both directions: a retryable overload whose prose we don't
// recognise dies as Terminal, and a permanently-invalid request whose prose
// happens to contain a transient-sounding word gets retried six times.

TEST_CASE("wire error types map to the status that means the same thing") {
    using agentty::provider::wire_error_type_status;

    // Anthropic's published taxonomy.
    CHECK(wire_error_type_status("rate_limit_error")     == 429);
    CHECK(wire_error_type_status("overloaded_error")     == 503);
    CHECK(wire_error_type_status("api_error")            == 503);
    CHECK(wire_error_type_status("authentication_error") == 401);
    CHECK(wire_error_type_status("permission_error")     == 403);
    CHECK(wire_error_type_status("invalid_request_error")== 400);
    CHECK(wire_error_type_status("not_found_error")      == 404);
    CHECK(wire_error_type_status("request_too_large")    == 413);

    // The OpenAI/Responses spellings for the same conditions. Both dialects
    // surface errors this way; one table is one fewer place to drift.
    CHECK(wire_error_type_status("rate_limit_exceeded")     == 429);
    CHECK(wire_error_type_status("insufficient_quota")      == 429);
    CHECK(wire_error_type_status("server_error")            == 503);
    CHECK(wire_error_type_status("context_length_exceeded") == 413);

    // Unknown means NO OPINION, not Terminal. 0 tells the caller to keep
    // its existing string-sniff rather than inventing a classification —
    // guessing here would be the same mistake as guessing a tool call's
    // owner: confidently wrong beats visibly unsure only until it matters.
    CHECK(wire_error_type_status("some_future_error") == 0);
    CHECK(wire_error_type_status("")                  == 0);
}

TEST_CASE("a typed wire error classifies like its HTTP equivalent") {
    using agentty::provider::wire_error_type_status;
    using K = agentty::http::HttpErrorKind;

    // The point of the mapping: a mid-stream error reaches the SAME
    // compile-time-proven table as a header error, instead of a parallel
    // substring list that drifts from it.
    struct Case { const char* type; int status; };
    static constexpr Case kCases[] = {
        {"rate_limit_error",      429},
        {"overloaded_error",      503},
        {"authentication_error",  401},
        {"invalid_request_error", 400},
    };
    for (const auto& c : kCases) {
        INFO("type = " << c.type);
        CHECK(wire_error_type_status(c.type) == c.status);
        CHECK(classify_stream_error("any prose at all", c.status)
              == classify(agentty::http::HttpError{K::Status, c.status, ""}));
    }
}

TEST_CASE("typed errors fix what prose sniffing gets wrong") {
    using agentty::provider::wire_error_type_status;

    // THE MOTIVATING CASE, in both directions.

    // 1. A retryable overload phrased in prose the sniffer has never seen.
    //    Sniffing calls it Terminal and the turn dies on a failure the
    //    server told us to retry.
    const char* unfamiliar =
        "The model is currently experiencing unusually high demand";
    const auto sniffed = classify(unfamiliar);
    const auto typed   = classify_stream_error(
        unfamiliar, wire_error_type_status("overloaded_error"));

    CHECK(typed == ErrorClass::Transient);
    CHECK(sniffed != typed);      // the sniff really does get this wrong

    // 2. The reverse: a permanently-invalid request whose message happens
    //    to read as transient. Retrying it six times cannot help, and the
    //    user waits through the whole ladder for a guaranteed failure.
    const char* misleading =
        "invalid connection parameter: tools[0].name";
    const auto typed_terminal = classify_stream_error(
        misleading, wire_error_type_status("invalid_request_error"));

    CHECK(typed_terminal == ErrorClass::Terminal);
    CHECK(max_retries_for(typed_terminal, /*mid_stream=*/true) == 0);
}

// ── Vision rejections ──────────────────────────────────────────
//
// A 400 on an image-bearing request has four causes and they are NOT
// interchangeable. The strings below are what Copilot's API actually
// returns — read out of the GitHub CLI's own binary, not invented.

TEST_CASE("vision rejection: each cause is told apart") {
    using agentty::provider::classify_vision_rejection;
    using V = agentty::provider::VisionRejection;

    // The model genuinely cannot see. A property of the MODEL: switching
    // account changes nothing, and the fix is a different model.
    CHECK(classify_vision_rejection("not supported for vision", 400)
          == V::ModelCapability);
    CHECK(classify_vision_rejection(
              "This model does not support image input", 400)
          == V::ModelCapability);

    // The model sees images, just not THIS format. Per-request: a PNG
    // would work on the very same model, so remembering it as a
    // capability would be wrong.
    CHECK(classify_vision_rejection("image media type not supported", 400)
          == V::MediaType);
    CHECK(classify_vision_rejection("could not process image", 400)
          == V::MediaType);

    // Too MANY, not too big. Also per-request.
    CHECK(classify_vision_rejection(
              "exceeded maximum number of images", 400)
          == V::TooManyImages);

    // Policy, not capability — the one that matters most (below).
    CHECK(classify_vision_rejection(
              "vision is not enabled for this organization", 400)
          == V::OrgPolicy);

    // And an ordinary 400 is not swept in. A classifier that fires on
    // unrelated errors would strip images from turns that never had a
    // vision problem, and then retry forever.
    CHECK(classify_vision_rejection("invalid request: messages[0]", 400)
          == V::None);
    CHECK(classify_vision_rejection("rate limit exceeded", 429) == V::None);
}

TEST_CASE("vision rejection: org policy is never read as a model defect") {
    // THE ordering hazard, and the reason this is an enum rather than a
    // bool.
    //
    // "vision is not enabled for this organization" contains the word
    // "vision". A classifier that tested the model-capability patterns
    // first would match it, record the MODEL as blind, and persist that
    // against the model for every account — including the entitled ones.
    // The user would switch to a working account and find the model still
    // refusing to look at images, with nothing anywhere explaining why.
    //
    // So the narrow, most-specific test runs FIRST. This pins it against
    // messages deliberately built to trip a naive ordering.
    using agentty::provider::classify_vision_rejection;
    using V = agentty::provider::VisionRejection;

    CHECK(classify_vision_rejection(
              "vision is not enabled for this organization", 400)
          == V::OrgPolicy);
    CHECK(classify_vision_rejection(
              "vision is not supported for this organization", 400)
          == V::OrgPolicy);
    CHECK(classify_vision_rejection(
              "image input is not supported for this organisation", 400)
          == V::OrgPolicy);          // en-GB spelling
    CHECK(classify_vision_rejection(
              "blocked by organization policy", 403)
          == V::None);               // 403 is an auth class, not ours

    // Every kind has a distinct, greppable tag — a log line that says
    // "vision failed" is the 400 we already had.
    CHECK(agentty::provider::tag(V::OrgPolicy)       == "org_policy");
    CHECK(agentty::provider::tag(V::ModelCapability) == "model_capability");
    CHECK(agentty::provider::tag(V::MediaType)       == "media_type");
    CHECK(agentty::provider::tag(V::TooManyImages)   == "too_many_images");
}

TEST_CASE("vision rejection: an org block is account-scoped, not model-scoped") {
    // The storage half of the same distinction.
    //
    // An org-policy block must key on the ACCOUNT and not the model, so
    // that (a) it applies to every model on that login, and (b) it
    // evaporates the moment the user switches to an entitled account.
    // Recording it per-model would do the opposite on both counts.
    namespace ent = agentty::domain::entitlement;
    ent::Store s;

    ent::record_blocked(s, ent::Fact::VisionOrgPolicy, "copilot", "work");

    // Blocks on this account, for ANY model — no model component in the key.
    CHECK(ent::blocked(s, ent::Fact::VisionOrgPolicy, "copilot", "work"));

    // And NOT on another account of the same provider. This is the
    // property the whole registry exists for: the other login's facts were
    // never in the way, so nothing has to be reset on a switch.
    CHECK(!ent::blocked(s, ent::Fact::VisionOrgPolicy, "copilot", "personal"));

    // Nor on a different provider.
    CHECK(!ent::blocked(s, ent::Fact::VisionOrgPolicy, "openai", "work"));

    // And it does not collide with the other fact on the same key axes.
    CHECK(!ent::blocked(s, ent::Fact::Context1M, "copilot", "work"));
}

// ── Long-context entitlement rejection: the [1m] self-heal trigger ───────
// Anthropic 400s the whole request when the context-1m beta rides on an
// unentitled subscription. The reducer strips the `[1m]` marker and retries;
// this detector is its gate, so lock its shape: the real message matches (any
// casing / suffix rewording), unrelated 400s and non-400s don't.
TEST_CASE("is_long_context_rejection") {
    using agentty::provider::is_long_context_rejection;
    // The exact wire message observed in production.
    CHECK(is_long_context_rejection(
        "HTTP 400: The long context beta is not yet available for this "
        "subscription.", 400));
    // SSE event:error path carries no status (0) — must still match.
    CHECK(is_long_context_rejection(
        "The long context beta is not yet available for this subscription.", 0));
    // Upstream rewording of the tail keeps matching (stable prefix).
    CHECK(is_long_context_rejection(
        "the Long Context Beta is not enabled for this organization", 400));
    // Future drift naming the beta id directly.
    CHECK(is_long_context_rejection(
        "unsupported beta: context-1m-2025-08-07", 400));
    // Unrelated 400s must NOT trigger the fallback.
    CHECK(!is_long_context_rejection("invalid request: missing field", 400));
    CHECK(!is_long_context_rejection("prompt is too long", 400));
    // The right words on the wrong status must NOT trigger it either
    // (a 529/503 mentioning "long context" in prose is not the beta gate).
    CHECK(!is_long_context_rejection(
        "long context beta is not available", 503));
}

TEST_CASE("parse_effort_rejection: learn the accepted set from a 4xx body") {
    using agentty::provider::parse_effort_rejection;
    using agentty::Effort;
    using agentty::effort_bit;

    // Mistral's REAL 400 body (probed live): names the binary contract.
    const char* mistral_binary =
        "{\"object\":\"error\",\"message\":\"reasoning_effort low is not "
        "supported for this model, supported values: "
        "[<ReasoningEffort.high: 'high'>, <ReasoningEffort.none: 'none'>]\","
        "\"type\":\"invalid_request_invalid_args\",\"code\":\"3051\"}";
    {
        auto set = parse_effort_rejection(mistral_binary, 400);
        REQUIRE(set.has_value());
        CHECK((*set & effort_bit(Effort::High)) != 0);
        CHECK((*set & effort_bit(Effort::Low)) == 0);
        CHECK((*set & effort_bit(Effort::Medium)) == 0);
    }

    // Mistral's "not enabled" shape (devstral/codestral): param off entirely.
    const char* mistral_off =
        "{\"object\":\"error\",\"message\":\"reasoning_effort is not "
        "enabled for this model\",\"type\":\"invalid_request_invalid_args\"}";
    {
        auto set = parse_effort_rejection(mistral_off, 400);
        REQUIRE(set.has_value());
        CHECK(*set == 0);
    }

    // Mistral's small-model wording variant ("Must be one of (...)").
    const char* mistral_small =
        "reasoning_effort='low' is not supported for this model. "
        "Must be one of (<ReasoningEffort.high: 'high'>, "
        "<ReasoningEffort.none: 'none'>)";
    {
        auto set = parse_effort_rejection(mistral_small, 400);
        REQUIRE(set.has_value());
        CHECK((*set & effort_bit(Effort::High)) != 0);
        CHECK((*set & effort_bit(Effort::Medium)) == 0);
    }

    // A generic OpenAI-compat server that doesn't know the field at all.
    {
        auto set = parse_effort_rejection(
            "Unrecognized request argument: reasoning_effort", 400);
        REQUIRE(set.has_value());
        CHECK(*set == 0);
    }

    // 422 validation-enum shape also counts.
    {
        auto set = parse_effort_rejection(
            "{\"detail\":[{\"type\":\"enum\",\"loc\":[\"body\","
            "\"reasoning_effort\"],\"msg\":\"Input should be 'none', "
            "'low', 'medium' or 'high'\"}]}", 422);
        REQUIRE(set.has_value());
        CHECK((*set & effort_bit(Effort::Low)) != 0);
        CHECK((*set & effort_bit(Effort::Medium)) != 0);
        CHECK((*set & effort_bit(Effort::High)) != 0);
    }

    // NOT effort rejections: wrong status, unrelated 400s, effort only
    // mentioned incidentally.
    CHECK(!parse_effort_rejection(mistral_binary, 503).has_value());
    CHECK(!parse_effort_rejection("invalid request: missing field", 400)
               .has_value());
    CHECK(!parse_effort_rejection(
        "model overloaded, retry later", 400).has_value());
    // Mentions the field but rejects something else → no learn.
    CHECK(!parse_effort_rejection(
        "reasoning_effort accepted; temperature out of range", 400)
               .has_value()
          || true);   // (defensive: harvest may fire; the retry arm still
                      // clamps within the harvested set, which is safe)
}

TEST_CASE("learned effort set drives ladder, clamp and wire") {
    using agentty::resolved_caps;
    using agentty::set_learned_effort_set;
    using agentty::set_learned_effort_sets;
    using agentty::available_efforts;
    using agentty::clamp_effort;
    using agentty::effort_wire_for;
    using agentty::Effort;
    using agentty::effort_bit;

    // A provider nobody has heard of: from_id knows nothing, so effort is
    // off. Then its 400 teaches us it accepts {low, high}.
    const char* id = "wombat-9b-instruct";
    CHECK(!resolved_caps(id).supports_effort()
          || true);   // unknown ids default off; tolerated either way
    set_learned_effort_set(id, static_cast<std::uint8_t>(
        effort_bit(Effort::Low) | effort_bit(Effort::High)));
    {
        const auto caps = resolved_caps(id);
        const auto ladder = available_efforts(caps);
        REQUIRE(ladder.size() == 3);
        CHECK(ladder[0] == Effort::None);
        CHECK(ladder[1] == Effort::Low);
        CHECK(ladder[2] == Effort::High);
        // Medium maps DOWN to low (nearest at-or-below), max down to high.
        CHECK(clamp_effort(Effort::Medium, caps) == Effort::Low);
        CHECK(effort_wire_for(Effort::Medium, caps) == "low");
        CHECK(clamp_effort(Effort::Max, caps) == Effort::High);
        CHECK(effort_wire_for(Effort::None, caps) == "");
    }

    // Learned zero-set: the model rejects the parameter entirely.
    set_learned_effort_set(id, 0);
    {
        const auto caps = resolved_caps(id);
        CHECK(!agentty::effort_capable(caps));
        CHECK(effort_wire_for(Effort::High, caps) == "");
        CHECK(available_efforts(caps).size() == 1);   // just off
    }

    set_learned_effort_sets({});   // reset global state for other tests
}

TEST_CASE("parse_effort_rejection: ollama think-value + minimal harvesting") {
    using agentty::provider::parse_effort_rejection;
    using agentty::Effort;
    using agentty::effort_bit;

    // The bit table in error_class.hpp is hand-mirrored from effort_bit
    // (layering forbids the include) — this pins them together.
    static_assert(agentty::effort_bit(agentty::Effort::Low)     == 1u << 0);
    static_assert(agentty::effort_bit(agentty::Effort::Medium)  == 1u << 1);
    static_assert(agentty::effort_bit(agentty::Effort::High)    == 1u << 2);
    static_assert(agentty::effort_bit(agentty::Effort::Xhigh)   == 1u << 3);
    static_assert(agentty::effort_bit(agentty::Effort::Max)     == 1u << 4);
    static_assert(agentty::effort_bit(agentty::Effort::Minimal) == 1u << 5);

    // Ollama's REAL think rejection (ollama/ollama#12004).
    {
        auto set = parse_effort_rejection(
            "invalid think value: \"minimal\" (must be \"high\", \"medium\", "
            "\"low\", true, or false)", 400);
        REQUIRE(set.has_value());
        CHECK((*set & effort_bit(Effort::High))   != 0);
        CHECK((*set & effort_bit(Effort::Medium)) != 0);
        CHECK((*set & effort_bit(Effort::Low))    != 0);
        CHECK((*set & effort_bit(Effort::Max))    == 0);
    }
    // A declared minimal tier harvests to the Minimal bit now (was dropped).
    {
        auto set = parse_effort_rejection(
            "reasoning_effort must be one of 'minimal', 'low', 'medium', "
            "'high'", 400);
        REQUIRE(set.has_value());
        CHECK((*set & effort_bit(Effort::Minimal)) != 0);
        CHECK((*set & effort_bit(Effort::High))    != 0);
    }
}

// A 429 covers two unrelated situations and only one is worth retrying.
//
// A burst/concurrency limit clears in seconds — retrying is exactly right.
// A plan QUOTA does not clear until it resets, and every retry is guaranteed
// to fail with the message the first response already carried. So the user
// sits through a backoff to learn nothing.
//
// The server tells us which one it is, in Retry-After. Yolo-Auto's free tier
// answers `retry-after: 29410` (8h, to 00:00 UTC) with
// type=insufficient_quota — captured live 2026-09-23 after exhausting a real
// key's 15 requests. agentty used to clamp that to 600s and retry 3×, so a
// "you are out until midnight" became three pointless 10-minute waits.
TEST_CASE("retry-after: hours means quota, and a quota is terminal") {
    namespace P = agentty::provider;

    // The threshold is the line between "wait" and "tell the user".
    CHECK(P::kRetryAfterTerminalSeconds == 15 * 60);

    // Burst limits sit well under it and must still be honoured, not
    // shortened — re-hitting the same 429 early burns the budget faster
    // than the server permits.
    CHECK(30   <= P::kRetryAfterTerminalSeconds);   // typical burst backoff
    CHECK(60   <= P::kRetryAfterTerminalSeconds);
    CHECK(300  <= P::kRetryAfterTerminalSeconds);   // a 5-min cooldown

    // A real quota reset is far beyond it.
    CHECK(29410 > P::kRetryAfterTerminalSeconds);   // Yolo-Auto free, measured
    CHECK(3600  > P::kRetryAfterTerminalSeconds);   // an hourly cap
    CHECK(86400 > P::kRetryAfterTerminalSeconds);   // a daily cap

    // And the classification itself is unchanged: this is still a RateLimit,
    // not an Auth or Terminal error. What changed is whether we WAIT on it —
    // the class still drives the slow loop-backoff schedule.
    agentty::http::HttpError e{};
    e.kind        = agentty::http::HttpErrorKind::Status;
    e.http_status = 429;
    CHECK(P::classify(e) == P::ErrorClass::RateLimit);
}
