#pragma once
// agentty::util::identity — the ONE place agentty states who it is to a
// remote service.
//
// ────────────────────────────────────────────────────────────────────
// Why this file exists
// ────────────────────────────────────────────────────────────────────
// App attribution (issue #74) turned agentty's name and homepage from
// cosmetic strings into values a REMOTE SERVICE keys records on: OpenRouter
// credits a request to an app page identified by the URL we send, and some
// models only serve a client that names itself. A fact a vendor builds a
// record around deserves exactly one definition, not a literal typed into
// whichever transport needed it that day.
//
// Scope: this is agentty describing ITSELF — a name and a URL. It is not a
// place for per-vendor protocol headers. Those belong on the provider row
// (ProviderDescriptor::attribution) or, for request-shaped junk like
// Copilot's editor block, on Endpoint::extra_headers.
//
// Not here (deliberately): the User-Agent string. "agentty/<version>" is
// still built from the AGENTTY_VERSION macro at its nine call sites, because
// that macro lives in io/http.hpp and hoisting it would mean http.hpp
// depending on util/ or this file duplicating the fallback. Worth doing, but
// as its own change — not smuggled in behind an attribution fix.

#include <string_view>

namespace agentty::util::identity {

// The product name, as a human should see it in someone else's dashboard.
// Lowercase because that is how agentty is spelled everywhere else (binary,
// repo, docs); a vendor leaderboard showing "agentty" is correct, "Agentty"
// is a different product that does not exist.
inline constexpr std::string_view name = "agentty";

// The canonical home page. This is the identifier OpenRouter (and anything
// like it) keys an app page on, so it must be the real public URL and must
// stay stable — changing it does not rename an app page, it creates a new
// one and orphans the old. https, no trailing slash.
inline constexpr std::string_view url = "https://agentty.org";

}  // namespace agentty::util::identity
