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

// A Routes whose two arms just record which one fired.
struct Probe {
    std::string hit;
    provider::Routes routes() {
        return provider::Routes{
            .anthropic = [this](provider::Request, provider::EventSink) {
                hit = "anthropic";
            },
            .chatgpt = [this](provider::Request, provider::EventSink) {
                hit = "chatgpt";
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

int main() {
    test_anthropic_selection_hits_anthropic_route();
    test_chatgpt_selection_hits_chatgpt_route();
    test_is_chatgpt_predicate();

    if (g_failures == 0) {
        std::printf("dispatch_route_test: all checks passed\n");
        return 0;
    }
    std::printf("dispatch_route_test: %d failure(s)\n", g_failures);
    return 1;
}
