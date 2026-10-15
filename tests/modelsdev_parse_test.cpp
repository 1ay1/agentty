// modelsdev_parse_test — the models.dev loader must turn a document into the
// exact same catalog facts whatever JSON parser it uses.
//
// A checked-in fixture covers every field shape the loader reads; a second
// case runs over the real ~/.agentty cache when present and prints a
// fingerprint of every fact, so a parser swap can be checked against the
// full live document.

#include "agtest.hpp"

#include "agentty/domain/catalog.hpp"
#include "agentty/util/modelsdev.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using namespace agentty;

namespace {

// Every fact in a batch as sorted text lines. Order-independent, so two
// parsers that walk the document differently compare equal.
std::vector<std::string> facts_of(const CatalogBatch& b) {
    std::vector<std::string> out;
    for (const auto& [k, v] : b.reasoning.sets)   out.push_back("rs " + k + "=" + (v ? "1" : "0"));
    for (const auto& [k, v] : b.reasoning.merges) out.push_back("rm " + k + "=" + (v ? "1" : "0"));
    for (const auto& [k, v] : b.effort.sets)      out.push_back("es " + k + "=" + std::to_string(v));
    for (const auto& [k, v] : b.effort.merges)    out.push_back("em " + k + "=" + std::to_string(v));
    for (const auto& [k, v] : b.context.sets)     out.push_back("cs " + k + "=" + std::to_string(v));
    for (const auto& [k, v] : b.context.merges)   out.push_back("cm " + k + "=" + std::to_string(v));
    for (const auto& [h, m, e] : b.endpoints)
        out.push_back("ep " + h + " " + m + "=" + std::to_string(e.tokens) + ":" + std::to_string(e.port));
    std::sort(out.begin(), out.end());
    return out;
}

std::uint64_t fnv(const std::vector<std::string>& lines) {
    std::uint64_t h = 1469598103934665603ull;
    for (const auto& l : lines) {
        for (unsigned char c : l) { h ^= c; h *= 1099511628211ull; }
        h ^= '\n'; h *= 1099511628211ull;
    }
    return h;
}

constexpr const char* kFixture = R"({
  "acme": {
    "env": ["ACME_KEY", "ACME_ALT"],
    "api": "https://api.acme.test:8443/v1",
    "models": {
      "acme/think-3.5": {
        "reasoning": true,
        "limit": {"context": 200000, "output": 8192},
        "reasoning_options": [
          {"type": "effort", "values": ["low", "medium", "high", "ultra"]}
        ]
      },
      "acme-toggle": {
        "reasoning": true,
        "reasoning_options": [{"type": "toggle"}]
      },
      "acme-both": {
        "reasoning": true,
        "reasoning_options": [{"type": "toggle"}, {"type": "effort", "values": ["high"]}]
      },
      "acme-plain": {"reasoning": false, "limit": {"context": 32000}},
      "acme-no-reasoning-field": {"limit": {"context": 1000}},
      "acme-bad-context": {"reasoning": true, "limit": {"context": -5}},
      "acme-huge-context": {"reasoning": true, "limit": {"context": 99999999999}},
      "acme-float-context": {"reasoning": true, "limit": {"context": 1.5}},
      "acme-string-reasoning": {"reasoning": "yes"},
      "acme-empty-effort": {"reasoning": true,
        "reasoning_options": [{"type": "effort", "values": []}]},
      "acme-weird-opts": {"reasoning": true,
        "reasoning_options": ["x", {"type": 3}, {"values": ["low"]}]}
    }
  },
  "noenv": {
    "models": { "solo": {"reasoning": true, "limit": {"context": 4096}} }
  },
  "badenv": {
    "env": [42], "api": 7,
    "models": { "solo2": {"reasoning": false} }
  },
  "notobject": 5,
  "nomodels": {"env": ["X"]},
  "modelsnotobject": {"models": []},
  "localhost": {
    "api": "http://localhost:1234/v1",
    "models": { "local-m": {"reasoning": true, "limit": {"context": 8192}} }
  }
})";

}  // namespace

TEST_CASE("modelsdev parse: fixture yields the expected facts") {
    CatalogBatch b;
    const int n = modelsdev::parse_into(kFixture, b);
    const auto facts = facts_of(b);
    // Every model entry is counted, oddly typed ones included.
    CHECK(n == 14);
    const std::vector<std::string> want = {
        // Recorded from the nlohmann loader up to the entry that made it
        // throw (acme-weird-opts' numeric "type").
        "cm acme-plain=32000",
        "cs dev:acme/acme-plain=32000",
        "em acme-both=4",
        "em acme-toggle=4",
        "ep api.acme.test acme-plain=32000:8443",
        "es dev:acme/acme-both=4",
        "es dev:acme/acme-toggle=4",
        "rm acme-bad-context=1",
        "rm acme-both=1",
        "rm acme-empty-effort=1",
        "rm acme-float-context=1",
        "rm acme-huge-context=1",
        "rm acme-plain=0",
        "rm acme-toggle=1",
        "rs dev:acme/acme-bad-context=1",
        "rs dev:acme/acme-both=1",
        "rs dev:acme/acme-empty-effort=1",
        "rs dev:acme/acme-float-context=1",
        "rs dev:acme/acme-huge-context=1",
        "rs dev:acme/acme-plain=0",
        "rs dev:acme/acme-toggle=1",
    };
    // Everything the old loader produced must still be produced, unchanged.
    for (const auto& w : want)
        CHECK_MESSAGE(std::find(facts.begin(), facts.end(), w) != facts.end(), "missing: " << w);
}

TEST_CASE("modelsdev parse: one malformed entry doesn't cost the document") {
    // nlohmann threw on a numeric option "type" (and on a numeric provider
    // "api"), and the catch threw away every model. Now the bad field is
    // skipped and the rest of the document still lands.
    CatalogBatch b;
    (void)modelsdev::parse_into(kFixture, b);
    const auto facts = facts_of(b);
    const auto has = [&](std::string_view s) {
        return std::any_of(facts.begin(), facts.end(),
                           [&](const std::string& f) { return f.find(s) != std::string::npos; });
    };
    CHECK(has("acme-weird-opts"));          // the entry with the bad option
    CHECK(has("think-3-5"));                // models after it (keys are normalised)
    CHECK(has("solo"));                     // later providers
    CHECK(has("solo2"));                    // provider with a numeric api
    CHECK(has("local-m"));                  // the last provider
}

TEST_CASE("modelsdev parse: the real cache gives the same facts") {
    const char* home = std::getenv("HOME");
    if (!home) return;
    std::ifstream in(std::string{home} + "/.agentty/cache/modelsdev.json", std::ios::binary);
    if (!in) return;   // no cache on this machine: nothing to compare
    std::ostringstream ss;
    ss << in.rdbuf();
    CatalogBatch b;
    const int n = modelsdev::parse_into(ss.str(), b);
    const auto facts = facts_of(b);
    MESSAGE("models=" << n << " facts=" << facts.size() << " fnv=" << fnv(facts));
    CHECK(n > 0);
    // Fingerprint of the nlohmann loader's output on the cache file it was
    // recorded against. A refreshed cache changes it, so this only checks
    // when the file is that exact one (by size).
    if (ss.str().size() == 5623041) {
        CHECK(n == 8814);
        CHECK(facts.size() == 65355);
        CHECK(fnv(facts) == 11372656136564762570ull);
    }
}
