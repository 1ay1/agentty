// tests/host_probe_taxonomy_test.cpp — why a custom host failed, in words.
//
// The probe's job is not "did it work" but "what do I tell the user to do
// next". Three very different mistakes used to arrive as the same string:
//
//   200 + HTML   you pasted the dashboard URL, not the API base
//   401/403      the address is RIGHT, it wants a key
//   connect fail nothing is listening there
//
// Each has a different fix, so each needs a different sentence — and the 401
// case must not send you back to edit a URL you typed correctly.
//
// The HTML case is not hypothetical: https://yolo-auto.com/models returns
// HTTP 200 with a full SPA (verified live 2026-09-23), so a user who drops
// the /v1 hits 200-with-HTML and used to be told "HTTP 200 — no model list at
// any known path", which names neither the problem nor the fix.

#include <doctest/doctest.h>

#include <string>

#include "agentty/provider/openai/transport.hpp"

using agentty::provider::openai::HostProbe;

namespace {
HostProbe failed(HostProbe::Failure f, int status = 0) {
    HostProbe p;
    p.failure     = f;
    p.http_status = status;
    return p;
}
}  // namespace

TEST_CASE("probe: every failure explains itself, and no two read alike") {
    const auto unreachable = failed(HostProbe::Failure::Unreachable);
    const auto needs_key   = failed(HostProbe::Failure::NeedsKey, 401);
    const auto not_an_api  = failed(HostProbe::Failure::NotAnApi, 200);
    const auto no_list     = failed(HostProbe::Failure::NoModelList, 200);
    const auto http_err    = failed(HostProbe::Failure::HttpError, 502);

    // None of them is ok().
    CHECK(!unreachable.ok());
    CHECK(!needs_key.ok());
    CHECK(!not_an_api.ok());

    // Each says something DIFFERENT. A taxonomy whose arms produce the same
    // sentence is not a taxonomy.
    const std::string a = unreachable.explain(), b = needs_key.explain(),
                      c = not_an_api.explain(),  d = no_list.explain(),
                      e = http_err.explain();
    CHECK(a != b); CHECK(a != c); CHECK(a != d);
    CHECK(b != c); CHECK(b != d); CHECK(c != d); CHECK(d != e);
    for (const auto& s : {a, b, c, d, e}) CHECK(!s.empty());

    // And each names the ACTUAL fix.
    CHECK(a.find("nothing listening") != std::string::npos);
    CHECK(b.find("needs an API key") != std::string::npos);
    CHECK(b.find("endpoint is right") != std::string::npos);  // don't edit the URL
    CHECK(c.find("web page") != std::string::npos);
    CHECK(c.find("/v1") != std::string::npos);                // name the fix
    CHECK(d.find("no model list") != std::string::npos);

    // Success explains nothing, because there is nothing to fix.
    HostProbe good;
    good.dialect     = HostProbe::Dialect::OpenAiCompat;
    good.model_count = 2;
    CHECK(good.ok());
    CHECK(good.explain().empty());
}

TEST_CASE("probe: a 401 must not read as 'nothing there'") {
    // THE REGRESSION THIS PINS. An endpoint that answers 401 is CORRECT —
    // it answered. Reporting it like a connect failure sends the user to
    // re-type an address that was already right, which is exactly backwards
    // for the most common paid-provider onboarding path (type host → 401 →
    // paste key). Verified live: https://yolo-auto.com/v1/models returns 401
    // "Missing API key" with no credentials.
    const auto needs_key = failed(HostProbe::Failure::NeedsKey, 401);
    const auto dead      = failed(HostProbe::Failure::Unreachable);

    CHECK(needs_key.explain().find("nothing listening") == std::string::npos);
    CHECK(dead.explain().find("API key") == std::string::npos);

    // 403 is the same class — Yolo-Auto returns it for a model the plan
    // does not include, on an otherwise perfectly good endpoint.
    const auto forbidden = failed(HostProbe::Failure::NeedsKey, 403);
    CHECK(forbidden.explain().find("403") != std::string::npos);
    CHECK(forbidden.explain().find("endpoint is right") != std::string::npos);
}

TEST_CASE("probe: 200-with-HTML is diagnosed as a web page, not a JSON error") {
    // The dashboard-URL mistake. The old code saw 200, tried to parse, failed,
    // and reported the 200 — telling the user the server was fine and giving
    // them nothing to change. The fix has to be NAMED, because "use the API
    // base URL" is not something a user guesses.
    const auto html = failed(HostProbe::Failure::NotAnApi, 200);
    CHECK(html.explain().find("web page") != std::string::npos);
    CHECK(html.explain().find("/v1") != std::string::npos);

    // And it is distinct from "reachable but no model list", which is a
    // genuinely different situation (a JSON API on the wrong prefix).
    const auto json_but_wrong = failed(HostProbe::Failure::NoModelList, 200);
    CHECK(html.explain() != json_but_wrong.explain());
}

TEST_CASE("probe: success carries what the UI shows") {
    // "✓ 12 models · openai · 45ms" — the it-just-works moment. Every piece of
    // that line comes from the probe, so all of it must survive.
    HostProbe p;
    p.dialect     = HostProbe::Dialect::OpenAiCompat;
    p.models_path = "/v1/models";
    p.model_count = 12;
    p.latency_ms  = 45;
    p.http_status = 200;

    CHECK(p.ok());
    CHECK(p.failure == HostProbe::Failure::None);
    CHECK(p.model_count == 12);
    CHECK(p.latency_ms == 45);
    CHECK(p.models_path == "/v1/models");

    // A bare Ollama daemon answers a different path and is still a success —
    // the probe ADOPTS what it found rather than asking the user to declare it.
    HostProbe ol;
    ol.dialect     = HostProbe::Dialect::OllamaNative;
    ol.models_path = "/api/tags";
    ol.model_count = 3;
    CHECK(ol.ok());
    CHECK(ol.explain().empty());
}
