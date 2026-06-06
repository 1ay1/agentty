#include "agentty/tool/spec.hpp"
#include "agentty/tool/tools.hpp"
#include "agentty/tool/util/arg_reader.hpp"
#include "agentty/tool/util/tool_args.hpp"
#include "agentty/tool/util/utf8.hpp"
#include "agentty/io/http.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <expected>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

namespace agentty::tools {

using json = nlohmann::json;

namespace {

using http::HttpMethod;

HttpMethod parse_method(std::string_view m) {
    if (m == "HEAD") return HttpMethod::Head;
    if (m == "POST") return HttpMethod::Post;
    return HttpMethod::Get;
}

struct WebFetchArgs {
    std::string url;
    HttpMethod method;
    std::vector<std::pair<std::string, std::string>> headers;
    std::string display_description;
};

std::expected<WebFetchArgs, ToolError> parse_web_fetch_args(const json& j) {
    util::ArgReader ar(j);
    auto url_opt = ar.require_str("url");
    if (!url_opt)
        return std::unexpected(ToolError::invalid_args("url required"));
    std::string url = *std::move(url_opt);
    // TLS-only by contract (the tool description says "https only" and
    // the transport below assumes 443/TLS). Reject anything else here so
    // the error surfaces at the arg layer with one clear message instead
    // of passing http:// through to parse_url which rejects it with a
    // different string.
    if (!url.starts_with("https://"))
        return std::unexpected(ToolError::invalid_args(
            "url must start with https:// (web_fetch is TLS-only)"));
    std::vector<std::pair<std::string, std::string>> hdrs;
    if (const json* h = ar.raw("headers"); h && h->is_object()) {
        for (auto& [k, v] : h->items()) {
            if (v.is_string()) hdrs.emplace_back(k, v.get<std::string>());
            else               hdrs.emplace_back(k, v.dump());
        }
    }
    return WebFetchArgs{
        std::move(url),
        parse_method(ar.str("method", "GET")),
        std::move(hdrs),
        ar.str("display_description", ""),
    };
}

// Cap the response body so a chatty server can't blow the model's
// context. 20 KB matches the per-tool budget CC keeps for Grep
// (binary near offset 80359109) and is enough for a typical README,
// API doc page, or short article. A long-form article that needs
// more than 20 KB is almost certainly chrome (nav, ads, sidebars,
// inline scripts) which the model wouldn't benefit from anyway.
// Previous cap was 200 KB — a single fetch could swallow ~28 % of a
// 200 K-token context window in one tool call.
constexpr size_t kMaxFetchBytes = 20'000;

// Parse https://host[:port]/path. http:// is rejected at the arg-validation
// layer above (TLS-only), but we still need to handle the `:port` suffix and
// query strings cleanly.
struct ParsedUrl {
    std::string host;
    uint16_t port = 443;
    std::string path = "/";
};

std::expected<ParsedUrl, std::string> parse_url(std::string_view url) {
    constexpr std::string_view k = "https://";
    if (!url.starts_with(k)) return std::unexpected(std::string{"missing https:// scheme"});
    url.remove_prefix(k.size());
    auto slash = url.find('/');
    auto authority = url.substr(0, slash);
    ParsedUrl out;
    out.path = (slash == std::string_view::npos) ? "/" : std::string{url.substr(slash)};
    if (auto colon = authority.find(':'); colon != std::string_view::npos) {
        out.host.assign(authority.substr(0, colon));
        try {
            out.port = static_cast<uint16_t>(std::stoi(std::string{authority.substr(colon + 1)}));
        } catch (...) { return std::unexpected(std::string{"bad port"}); }
    } else {
        out.host.assign(authority);
    }
    if (out.host.empty()) return std::unexpected(std::string{"empty host"});
    return out;
}

// SSRF guard. web_fetch is reachable by the model and, under the Write
// profile, runs without a permission prompt. Block hosts that would let
// it read the loopback interface, the link-local cloud-metadata endpoint
// (169.254.169.254 / fd00:ec2::254), or RFC1918 private ranges. This is
// a best-effort string/literal-IP check at the host level; it does not
// resolve DNS (a hostname that resolves to a private IP via rebinding is
// out of scope for this layer), but it closes the obvious direct-literal
// and localhost vectors.
[[nodiscard]] bool is_blocked_host(std::string_view host) {
    // Strip an IPv6 bracket form [::1] -> ::1
    if (host.size() >= 2 && host.front() == '[' && host.back() == ']')
        host = host.substr(1, host.size() - 2);

    std::string h{host};
    for (char& c : h) c = static_cast<char>(std::tolower(c));

    // Hostname-based loopback / metadata aliases.
    if (h == "localhost" || h.ends_with(".localhost")) return true;
    if (h == "metadata" || h == "metadata.google.internal") return true;
    if (h == "0") return true;            // 0 -> 0.0.0.0

    // IPv6 loopback / unspecified / unique-local / link-local.
    if (h == "::1" || h == "::") return true;
    if (h.starts_with("fc") || h.starts_with("fd")) return true;  // fc00::/7 ULA
    if (h.starts_with("fe80:") || h.starts_with("fe8")
        || h.starts_with("fe9") || h.starts_with("fea")
        || h.starts_with("feb")) return true;                    // fe80::/10

    // IPv4 dotted-quad parse. Anything that isn't four numeric octets
    // falls through (treated as a public hostname).
    unsigned a = 0, b = 0, c = 0, d = 0;
    if (std::sscanf(h.c_str(), "%u.%u.%u.%u", &a, &b, &c, &d) == 4
        && a < 256 && b < 256 && c < 256 && d < 256) {
        if (a == 127) return true;                       // 127.0.0.0/8 loopback
        if (a == 0)   return true;                       // 0.0.0.0/8
        if (a == 10)  return true;                       // 10.0.0.0/8
        if (a == 169 && b == 254) return true;           // link-local + metadata
        if (a == 172 && b >= 16 && b <= 31) return true; // 172.16.0.0/12
        if (a == 192 && b == 168) return true;           // 192.168.0.0/16
        if (a == 100 && b >= 64 && b <= 127) return true;// 100.64.0.0/10 CGNAT
        if (a >= 224) return true;                       // multicast / reserved
    }
    return false;
}

ExecResult run_web_fetch(const WebFetchArgs& a) {
    auto u = parse_url(a.url);
    if (!u) return std::unexpected(
        ToolError::invalid_args("could not parse url: " + a.url + " (" + u.error() + ")"));

    if (is_blocked_host(u->host))
        return std::unexpected(ToolError::invalid_args(
            "web_fetch refused: '" + u->host + "' is a loopback, private, "
            "or link-local/metadata address. Fetching internal endpoints is "
            "blocked (SSRF protection)."));

    http::Request req;
    req.method = a.method;
    req.host = u->host;
    req.port = u->port;
    req.path = u->path;
    req.headers.push_back({"user-agent", "agentty/" AGENTTY_VERSION});
    for (const auto& [k, v] : a.headers) {
        std::string lower; lower.reserve(k.size());
        for (char c : k) lower.push_back(static_cast<char>(std::tolower(c)));
        req.headers.push_back({std::move(lower), v});
    }

    http::Timeouts tos{
        .connect = std::chrono::milliseconds(10'000),
        .total   = std::chrono::milliseconds(30'000),
    };
    auto r = http::default_client().send(req, tos);
    if (!r) return std::unexpected(ToolError::network("fetch failed: " + r.error().render()));

    std::string content_type;
    for (const auto& h : r->headers)
        if (h.name == "content-type") { content_type = h.value; break; }

    std::string body = std::move(r->body);
    bool truncated = false;
    if (body.size() > kMaxFetchBytes) {
        // UTF-8-safe byte cap so we don't split a multi-byte sequence at the
        // boundary, then scrub — arbitrary HTTP bodies may not even be UTF-8
        // and json::dump() would throw on any invalid byte downstream.
        body.resize(util::safe_utf8_cut(body, kMaxFetchBytes));
        truncated = true;
    }
    body = util::to_valid_utf8(std::move(body));

    std::ostringstream out;
    out << "HTTP " << r->status;
    if (!content_type.empty()) out << " (" << content_type << ")";
    out << "\n\n" << body;
    if (truncated) out << "\n[body truncated at 20KB]";
    std::string s = out.str();
    if (!a.display_description.empty())
        s = a.display_description + "\n" + s;
    return ToolOutput{std::move(s), std::nullopt};
}

} // namespace

ToolDef tool_web_fetch() {
    ToolDef t;
    constexpr const auto& kSpec = spec::require<"web_fetch">();
    t.name = ToolName{std::string{kSpec.name}};
    t.description = "Fetch the contents of a URL. Supports HTTPS. Returns the response "
                    "body, status code, and content type. Use for documentation, APIs, etc.";
    t.input_schema = json{
        {"type","object"},
        {"required", {"url"}},
        {"properties", {
            {"display_description", {{"type","string"},
                {"description","One-line summary shown in the UI. Optional."}}},
            {"url",     {{"type","string"}, {"description","The URL to fetch (https only)"}}},
            {"method",  {{"type","string"}, {"description","HTTP method (default: GET)"}}},
            {"headers", {{"type","object"}, {"description","Additional headers as key-value pairs"}}},
        }},
    };
    t.effects = kSpec.effects;
    t.eager_input_streaming = kSpec.eager_input_streaming;
    t.execute = util::adapt<WebFetchArgs>(parse_web_fetch_args, run_web_fetch);
    return t;
}

} // namespace agentty::tools
