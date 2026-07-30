// dispatch_route_test — the payoff of the type-erased provider-routing seam.
//
// dispatch_stream(routes, selection, req, sink) is provider-agnostic: the two
// long-lived native providers are erased StreamFns. That lets us inject FAKE
// routes and assert exactly which one a given Selection reaches — no network,
// no concrete transport. This locks the routing table so a future edit can't
// silently send Anthropic turns to the OpenAI path (or vice versa).
//
// We can only cheaply observe the two erased routes (anthropic, chatgpt); the
// OpenAI-compat / Ollama / ACP arms construct real transports internally, so
// this test asserts the erased-route arms plus the is_chatgpt() predicate that
// selects between them within Kind::OpenAI.

#include <cstdio>
#include <memory>
#include <string>

#include "agentty/provider/dispatch.hpp"
#include "agentty/provider/prompt_policy.hpp"
#include "agentty/provider/selection.hpp"

using namespace agentty;

static int g_failures = 0;
#define CHECK(cond)                                                         \
    do {                                                                    \
        if (!(cond)) {                                                      \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);     \
            ++g_failures;                                                   \
        }                                                                   \
    } while (0)

// A router whose two long-lived arms just record which one fired. Each arm
// returns a DISTINCT StreamResult so the test can prove dispatch propagates the
// outcome value back through the erased seam (not just which arm fired).
struct Probe {
    std::string hit;
    provider::ProviderRouter router() {
        provider::ProviderRouter r;
        r.set(provider::LongLived::Anthropic,
              [this](provider::Request, provider::EventSink) {
                  hit = "anthropic";
                  provider::StreamResult sr;
                  sr.end  = provider::StreamEnd::CleanClose;
                  sr.stop = StopReason::EndTurn;
                  return sr;
              })
         .set(provider::LongLived::ChatGpt,
              [this](provider::Request, provider::EventSink) {
                  hit = "chatgpt";
                  provider::StreamResult sr;
                  sr.end   = provider::StreamEnd::TransportError;
                  sr.error = "chatgpt-boom";
                  return sr;
              });
        return r;
    }
};

static std::string route_for(const provider::Selection& sel) {
    Probe p;
    provider::dispatch_stream(p.router(), sel, provider::Request{},
                              provider::EventSink{});
    return p.hit;
}

// Same, but return the StreamResult dispatch handed back (the outcome value).
static provider::StreamResult result_for(const provider::Selection& sel) {
    Probe p;
    return provider::dispatch_stream(p.router(), sel, provider::Request{},
                                     provider::EventSink{});
}

static void test_anthropic_selection_hits_anthropic_route() {
    provider::Selection sel;                 // defaults to Kind::Anthropic
    sel.kind = provider::Kind::Anthropic;
    CHECK(route_for(sel) == "anthropic");
}

static void test_chatgpt_selection_hits_chatgpt_route() {
    provider::Selection sel;
    sel.kind = provider::Kind::OpenAI;
    sel.openai_endpoint.label = "chatgpt";   // the native OAuth Codex backend
    CHECK(sel.is_chatgpt());
    CHECK(route_for(sel) == "chatgpt");
}

static void test_is_chatgpt_predicate() {
    provider::Selection a;
    a.kind = provider::Kind::Anthropic;
    CHECK(!a.is_chatgpt());
    CHECK(!a.is_oauth_native());

    provider::Selection o;
    o.kind = provider::Kind::OpenAI;
    o.openai_endpoint.label = "openai";
    CHECK(!o.is_chatgpt());       // OpenAI-Kind but not the chatgpt label
    CHECK(!o.is_oauth_native());  // openai row is ApiKey, not oauth_native

    provider::Selection c;
    c.kind = provider::Kind::OpenAI;
    c.openai_endpoint.label = "chatgpt";
    CHECK(c.is_chatgpt());
    // is_oauth_native() is DATA-DRIVEN: it reads ProviderPreset::oauth_native
    // from the registry row, not a label compare. The chatgpt row is the only
    // one that sets the flag, so a second OAuth-native provider would just set
    // its flag and this predicate would light up with no code change here.
    CHECK(c.is_oauth_native());
}

// The redesign's core: which LONG-LIVED slot a selection routes to is derived
// purely from registry data (oauth_native flag + Anthropic dialect), never a
// label ladder. This locks that derivation so a future edit can't route a
// ChatGPT turn to the Anthropic transport or vice versa.
static void test_long_lived_slot_derivation() {
    provider::Selection anth;
    anth.kind = provider::Kind::Anthropic;
    CHECK(provider::long_lived_slot(anth) == provider::LongLived::Anthropic);

    provider::Selection chatgpt;
    chatgpt.kind = provider::Kind::OpenAI;
    chatgpt.openai_endpoint.label = "chatgpt";     // oauth_native row
    CHECK(provider::long_lived_slot(chatgpt) == provider::LongLived::ChatGpt);

    // A hosted OpenAI-compat backend has NO long-lived slot — it is built per
    // call from the Endpoint, so dispatch falls through to the transport path.
    provider::Selection groq;
    groq.kind = provider::Kind::OpenAI;
    groq.openai_endpoint.label = "groq";
    CHECK(provider::long_lived_slot(groq) == provider::LongLived::None);

    // An ACP subprocess is neither long-lived slot — it takes the ACP arm.
    provider::Selection acp;
    acp.kind = provider::Kind::ExternalAcp;
    acp.acp_agent_id = "claude-agent-acp";
    CHECK(provider::long_lived_slot(acp) == provider::LongLived::None);
}

// The prewarm ROUTING table, locked as a pure function (no socket opened).
// prewarm_target(sel) is registry-driven, so these assertions prove the
// warm-host derivation for every backend shape.
static void test_prewarm_target_table() {
    // Anthropic → the transport's fixed host from the registry row.
    {
        provider::Selection s;
        s.kind = provider::Kind::Anthropic;
        auto t = provider::prewarm_target(s);
        CHECK(t.should_warm());
        CHECK(t.host == "api.anthropic.com");
        CHECK(t.port == 443);
    }
    // ChatGPT → chatgpt.com (its Endpoint carries the port-0 sentinel, so the
    // registry prewarm_host wins).
    {
        provider::Selection s;
        s.kind = provider::Kind::OpenAI;
        s.openai_endpoint.label = "chatgpt";
        auto t = provider::prewarm_target(s);
        CHECK(t.should_warm());
        CHECK(t.host == "chatgpt.com");
        CHECK(t.port == 443);
    }
    // Hosted OpenAI-compat (groq) → warm its own Endpoint host.
    {
        provider::Selection s;
        s.kind = provider::Kind::OpenAI;
        s.openai_endpoint.label = "groq";
        s.openai_endpoint.host  = "api.groq.com";
        s.openai_endpoint.port  = 443;
        s.openai_endpoint.use_tls = true;
        auto t = provider::prewarm_target(s);
        CHECK(t.should_warm());
        CHECK(t.host == "api.groq.com");
    }
    // Local Ollama → nothing to warm.
    {
        provider::Selection s;
        s.kind = provider::Kind::OpenAI;
        s.openai_endpoint.label = "ollama";
        s.openai_endpoint.host  = "localhost";
        s.openai_endpoint.port  = 11434;
        s.openai_endpoint.use_tls = false;
        auto t = provider::prewarm_target(s);
        CHECK(!t.should_warm());
    }
    // ACP subprocess → no HTTP layer.
    {
        provider::Selection s;
        s.kind = provider::Kind::ExternalAcp;
        s.acp_agent_id = "claude-agent-acp";
        auto t = provider::prewarm_target(s);
        CHECK(!t.should_warm());
    }
    // Port-0 sentinel on a non-registry endpoint → skip (don't dial :0).
    {
        provider::Selection s;
        s.kind = provider::Kind::OpenAI;
        s.openai_endpoint.label = "custom";
        s.openai_endpoint.host  = "example.com";
        s.openai_endpoint.port  = 0;
        s.openai_endpoint.use_tls = true;
        auto t = provider::prewarm_target(s);
        CHECK(!t.should_warm());
    }
}

// The seam propagates the transport's StreamResult back to the caller — the
// outcome is a VALUE that survives the type-erased boundary, not just a side
// effect on the sink.
static void test_dispatch_propagates_stream_result() {
    provider::Selection anth;
    anth.kind = provider::Kind::Anthropic;
    auto ra = result_for(anth);
    CHECK(ra.end == provider::StreamEnd::CleanClose);
    CHECK(ra.ok());
    CHECK(!ra.cancelled());

    provider::Selection cg;
    cg.kind = provider::Kind::OpenAI;
    cg.openai_endpoint.label = "chatgpt";
    auto rc = result_for(cg);
    CHECK(rc.end == provider::StreamEnd::TransportError);
    CHECK(!rc.ok());
    CHECK(rc.error.has_value() && *rc.error == "chatgpt-boom");
}

// The PRECEDENCE heart of the termination layer, locked in isolation. A future
// edit that reordered these checks (e.g. moved the HTTP check above the cancel
// check) would silently turn a user's Esc during a 500 into a spurious
// "HTTP 500" error instead of a clean cancel. The fixed order is:
//   already-terminated > user-cancel > http-error > transport-error > clean.
static void test_classify_stream_end_precedence() {
    auto tok   = std::make_shared<agentty::http::CancelToken>();
    auto fresh = std::make_shared<agentty::http::CancelToken>();

    // A body that already fired its terminal wins over EVERYTHING else — even a
    // set cancel token and a 500 — so a clean early-abort is never a false error.
    tok->cancel();
    CHECK(provider::classify_stream_end(/*terminated=*/true, false, 500, tok)
          == provider::StreamEnd::AlreadyTerminated);

    // A set cancel token beats an HTTP status and a transport failure: a user
    // Esc mid-500 is a cancel, not an error surface.
    CHECK(provider::classify_stream_end(false, false, 500, tok)
          == provider::StreamEnd::UserCancelled);

    // No cancel → an HTTP >= 400 status beats a transport error.
    CHECK(provider::classify_stream_end(false, true, 503, fresh)
          == provider::StreamEnd::HttpError);

    // No cancel, no HTTP error, but the client returned !ok → transport error.
    CHECK(provider::classify_stream_end(false, false, 0, fresh)
          == provider::StreamEnd::TransportError);

    // Everything nominal → clean close (finish with the last-seen stop reason).
    CHECK(provider::classify_stream_end(false, true, 0, fresh)
          == provider::StreamEnd::CleanClose);
    // A 2xx status is not an error → still a clean close. A null cancel token is
    // treated as "not cancelled" (no crash on the nullptr).
    CHECK(provider::classify_stream_end(false, true, 200, nullptr)
          == provider::StreamEnd::CleanClose);
}

static void test_hosted_models_share_agent_prompt_policy() {
    const auto anthropic = provider::parse_selection("anthropic");
    const auto chatgpt   = provider::parse_selection("chatgpt");
    const auto local     = provider::parse_selection("llama.cpp");

    const std::string full = provider::system_prompt_for(anthropic);
    CHECK(!full.empty());
    CHECK(provider::system_prompt_for(chatgpt) == full);
    CHECK(provider::system_prompt_for(local) != full);
}

int main() {
    test_anthropic_selection_hits_anthropic_route();
    test_chatgpt_selection_hits_chatgpt_route();
    test_is_chatgpt_predicate();
    test_long_lived_slot_derivation();
    test_prewarm_target_table();
    test_dispatch_propagates_stream_result();
    test_classify_stream_end_precedence();
    test_hosted_models_share_agent_prompt_policy();

    if (g_failures == 0) {
        std::printf("dispatch_route_test: all checks passed\n");
        return 0;
    }
    std::printf("dispatch_route_test: %d failure(s)\n", g_failures);
    return 1;
}
