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

#include <memory>
#include <string>

#include "agtest.hpp"

#include "agentty/provider/dispatch.hpp"
#include "agentty/provider/prompt_policy.hpp"
#include "agentty/provider/selection.hpp"

using namespace agentty;

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

TEST_CASE("anthropic selection hits anthropic route") {
    // Through parse_selection: routing reads the registry row's .route
    // field, and a row-less Kind::Anthropic is unreachable in production.
    CHECK(route_for(provider::parse_selection("anthropic")) == "anthropic");
}

TEST_CASE("chatgpt selection hits chatgpt route") {
    // parse_selection resolves the registry row; identity travels ON the
    // selection rather than being re-derived from a mutable label.
    const auto sel = provider::parse_selection("chatgpt");
    CHECK(sel.is_chatgpt());
    CHECK(route_for(sel) == "chatgpt");
}

TEST_CASE("is_chatgpt predicate") {
    provider::Selection a;
    a.kind = provider::Kind::Anthropic;
    CHECK(!a.is_chatgpt());
    CHECK(!a.is_oauth_native());

    const auto o = provider::parse_selection("openai");
    CHECK(!o.is_chatgpt());       // OpenAI-Kind but not the chatgpt row
    CHECK(!o.is_oauth_native());  // openai row is ApiKey, not oauth_native

    const auto c = provider::parse_selection("chatgpt");
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
TEST_CASE("long lived slot derivation") {
    // The slot IS the registry row's .route field — resolve rows the way
    // production does (parse_selection), never hand-build.
    CHECK(provider::long_lived_slot(provider::parse_selection("anthropic"))
          == provider::LongLived::Anthropic);
    CHECK(provider::long_lived_slot(provider::parse_selection("chatgpt"))
          == provider::LongLived::ChatGpt);

    // A hosted OpenAI-compat backend has NO long-lived slot — it is built per
    // call from the Endpoint, so dispatch falls through to the transport path.
    CHECK(provider::long_lived_slot(provider::parse_selection("groq"))
          == provider::LongLived::None);

    // An ACP subprocess is neither long-lived slot — it takes the ACP arm.
    provider::Selection acp;
    acp.kind = provider::Kind::ExternalAcp;
    acp.acp_agent_id = "claude-agent-acp";
    CHECK(provider::long_lived_slot(acp) == provider::LongLived::None);
}

// The prewarm ROUTING table, locked as a pure function (no socket opened).
// prewarm_target(sel) is registry-driven, so these assertions prove the
// warm-host derivation for every backend shape.
TEST_CASE("prewarm target table") {
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
TEST_CASE("dispatch propagates stream result") {
    // parse_selection, not a hand-built Selection: routing reads the
    // registry row (sel.row->route), and a row-less Kind::Anthropic value
    // is a state production cannot produce.
    const auto anth = provider::parse_selection("anthropic");
    auto ra = result_for(anth);
    CHECK(ra.end == provider::StreamEnd::CleanClose);
    CHECK(ra.ok());
    CHECK(!ra.cancelled());

    const auto cg = provider::parse_selection("chatgpt");
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
TEST_CASE("classify stream end precedence") {
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

TEST_CASE("hosted models share agent prompt policy") {
    const auto anthropic = provider::parse_selection("anthropic");
    const auto chatgpt   = provider::parse_selection("chatgpt");
    const auto local     = provider::parse_selection("llama.cpp");

    const std::string full = provider::system_prompt_for(anthropic);
    CHECK(!full.empty());
    CHECK(provider::system_prompt_for(chatgpt) == full);
    CHECK(provider::system_prompt_for(local) != full);
}


// Auth capabilities live on the registry row, not in a chain of provider-name
// compares. Before this, login.cpp / modal.cpp / pickers.cpp held ~26 string
// comparisons against "anthropic" / "copilot" / "chatgpt" / "kimi" to decide
// which OAuth flow to run, whether to show the method menu, what to do with
// the cached auth header on account switch, and which model to default to.
// That is precisely the `if openai {} else if anthropic {}` shape the registry
// exists to delete: adding a provider meant grepping for its peers.
//
// These assertions pin the CAPABILITIES so a future row can't quietly
// contradict the code that reads them. The structural invariants themselves
// are static_asserts in registry.hpp (auth_caps_consistent) — this covers the
// semantic mapping those can't see.
TEST_CASE("provider registry carries the auth capabilities login reads") {
    using namespace agentty::provider;

    auto row = [](std::string_view id) -> const ProviderDescriptor& {
        const auto* p = preset_for(id);
        REQUIRE(p != nullptr);
        return *p;
    };

    // ── Anthropic: the only method menu (OAuth subscription vs API key) ──
    {
        const auto& p = row("anthropic");
        CHECK(p.method_menu);
        CHECK(p.oauth_proactive_refresh);
        CHECK(!p.device_login);
        CHECK(!p.token_in_transport);   // creds resolve to a real header
        CHECK(p.default_model == "claude-opus-4-5");
    }

    // ── ChatGPT: bespoke Codex flow, token held by its own transport ──
    // Also the row that used to claim is_local=true as a stand-in for "needs
    // no API key", which forced an `is_local && !oauth_native` workaround at
    // the one site wanting the literal meaning.
    {
        const auto& p = row("chatgpt");
        CHECK(p.oauth_native);
        CHECK(p.token_in_transport);
        CHECK(!p.device_login);         // oauth_native owns the launch path
        CHECK(!p.method_menu);
        CHECK(!p.is_local);             // chatgpt.com is not localhost
    }

    // ── Copilot / Kimi: the shared generic device launcher ──────────
    for (std::string_view id : {"copilot", "kimi"}) {
        const auto& p = row(id);
        CHECK(p.device_login);
        CHECK(p.token_in_transport);
        CHECK(!p.method_menu);
        CHECK(!p.is_local);
    }

    // ── Local backends never authenticate ───────────────────────────
    {
        const auto& p = row("ollama");
        CHECK(p.is_local);
        CHECK(!p.device_login);
        CHECK(!p.method_menu);
        CHECK(!p.token_in_transport);
    }

    // ── Keyed hosted providers: no OAuth machinery at all ───────────
    for (std::string_view id : {"groq", "mistral", "openrouter"}) {
        const auto& p = row(id);
        CHECK(p.auth == AuthStyle::ApiKey);
        CHECK(!p.device_login);
        CHECK(!p.method_menu);
        CHECK(!p.token_in_transport);
        CHECK(!p.oauth_proactive_refresh);
    }

    // ── is_local means LOCALHOST, exclusively ───────────────────────
    // The regression this guards: a remote provider marking itself local to
    // mean "keyless". AuthStyle::None already says that.
    for (const auto& p : providers())
        if (p.is_local)
            CHECK(p.host.empty() || p.host == "localhost");
}
