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
#include <string>

#include "agentty/provider/dispatch.hpp"
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

// A Routes whose two arms just record which one fired. Each arm returns a
// DISTINCT StreamResult so the test can prove dispatch propagates the outcome
// value back through the erased seam (not just which arm fired).
struct Probe {
    std::string hit;
    provider::Routes routes() {
        return provider::Routes{
            .anthropic = [this](provider::Request, provider::EventSink) {
                hit = "anthropic";
                provider::StreamResult r;
                r.end  = provider::StreamEnd::CleanClose;
                r.stop = StopReason::EndTurn;
                return r;
            },
            .chatgpt = [this](provider::Request, provider::EventSink) {
                hit = "chatgpt";
                provider::StreamResult r;
                r.end   = provider::StreamEnd::TransportError;
                r.error = "chatgpt-boom";
                return r;
            },
        };
    }
};

static std::string route_for(const provider::Selection& sel) {
    Probe p;
    provider::dispatch_stream(p.routes(), sel, provider::Request{},
                              provider::EventSink{});
    return p.hit;
}

// Same, but return the StreamResult dispatch handed back (the outcome value).
static provider::StreamResult result_for(const provider::Selection& sel) {
    Probe p;
    return provider::dispatch_stream(p.routes(), sel, provider::Request{},
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

    provider::Selection o;
    o.kind = provider::Kind::OpenAI;
    o.openai_endpoint.label = "openai";
    CHECK(!o.is_chatgpt());       // OpenAI-Kind but not the chatgpt label

    provider::Selection c;
    c.kind = provider::Kind::OpenAI;
    c.openai_endpoint.label = "chatgpt";
    CHECK(c.is_chatgpt());
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

int main() {
    test_anthropic_selection_hits_anthropic_route();
    test_chatgpt_selection_hits_chatgpt_route();
    test_is_chatgpt_predicate();
    test_prewarm_target_table();
    test_dispatch_propagates_stream_result();

    if (g_failures == 0) {
        std::printf("dispatch_route_test: all checks passed\n");
        return 0;
    }
    std::printf("dispatch_route_test: %d failure(s)\n", g_failures);
    return 1;
}
