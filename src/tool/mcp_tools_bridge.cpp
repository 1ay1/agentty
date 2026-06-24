// mcp_tools_bridge.cpp — see mcp_tools_bridge.hpp.
//
// Adapts mcp-cpp's make_provider() toolset into agentty ToolDefs. The
// provider owns each Tier-1 tool's implementation; this file owns the
// thin re-wrap: build a ToolDef per advertised tool, dispatch execute()
// into the provider, and decode the `_mcp_tools` meta (effects +
// FileChange) back into agentty's ToolOutput.

#include "agentty/tool/mcp_tools_bridge.hpp"

#include "agentty/diff/diff.hpp"
#include "agentty/io/http.hpp"

#include <mcp/tools/toolset.hpp>
#include <mcp/tools/host.hpp>
#include <mcp/tools/meta.hpp>
#include <mcp/tools/util/fs_helpers.hpp>
#include <mcp/cap/capability.hpp>
#include <mcp/cap/local.hpp>
#include <mcp/codec.hpp>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace agentty::tools {

namespace {

namespace mt = ::mcp::tools;

// ── HttpClient adapter ─────────────────────────────────────────────────
//   mcp's web tools call HttpClient::send(HttpRequest{url, ...}) and expect
//   the client to handle TLS + redirect-following + 4xx/5xx delivery as a
//   response (not a transport error). agentty's http::Client returns 4xx/5xx
//   as HttpError::Status and does NOT auto-follow redirects, so this adapter
//   parses the URL, follows 3xx manually, and maps status errors back into
//   an mcp HttpResponse the tool can read.

struct AgenttyHttpClient final : mt::HttpClient {
    static constexpr int kMaxRedirects = 5;

    struct Parsed { std::string host; uint16_t port = 443; std::string path = "/"; bool ok = false; };

    static Parsed parse(std::string_view url) {
        Parsed out;
        constexpr std::string_view k = "https://";
        if (!url.starts_with(k)) return out;
        url.remove_prefix(k.size());
        auto slash = url.find('/');
        auto authority = url.substr(0, slash);
        out.path = (slash == std::string_view::npos) ? "/" : std::string{url.substr(slash)};
        if (auto colon = authority.find(':'); colon != std::string_view::npos) {
            out.host.assign(authority.substr(0, colon));
            try { out.port = static_cast<uint16_t>(std::stoi(std::string{authority.substr(colon + 1)})); }
            catch (...) { return out; }
        } else {
            out.host.assign(authority);
        }
        out.ok = !out.host.empty();
        return out;
    }

    static std::string resolve_redirect(const Parsed& base, std::string_view loc) {
        while (!loc.empty() && (loc.front() == ' ' || loc.front() == '\t')) loc.remove_prefix(1);
        while (!loc.empty() && (loc.back() == ' ' || loc.back() == '\t')) loc.remove_suffix(1);
        if (loc.empty()) return {};
        if (loc.starts_with("https://")) return std::string{loc};
        if (loc.starts_with("http://")) return {};   // TLS-only
        if (loc.starts_with("//")) return "https:" + std::string{loc};
        std::string out = "https://" + base.host;
        if (base.port != 443) out += ":" + std::to_string(base.port);
        if (loc.starts_with("/")) { out += std::string{loc}; return out; }
        auto last = base.path.rfind('/');
        out += (last == std::string::npos) ? std::string{"/"} : base.path.substr(0, last + 1);
        out += std::string{loc};
        return out;
    }

    mt::HttpResponse send(const mt::HttpRequest& in) override {
        mt::HttpResponse out;
        std::string url = in.url;
        std::vector<std::string> visited;

        for (int hop = 0; hop <= kMaxRedirects; ++hop) {
            auto p = parse(url);
            if (!p.ok) { out.status = 0; out.error = "could not parse url: " + url; return out; }

            http::Request req;
            req.method = (in.method == "POST") ? http::HttpMethod::Post
                       : (in.method == "HEAD") ? http::HttpMethod::Head
                                               : http::HttpMethod::Get;
            req.host = p.host;
            req.port = p.port;
            req.path = p.path;
            req.body = in.body;
            for (const auto& [k, v] : in.headers) req.headers.push_back({k, v});

            http::Timeouts tos{
                .connect = std::chrono::milliseconds(10'000),
                .total   = std::chrono::milliseconds(30'000),
            };
            auto r = http::default_client().send(req, tos);
            if (!r) {
                // A 4xx/5xx arrives as HttpError::Status — surface it as a
                // real HttpResponse so the web tools can report the code.
                if (r.error().kind == http::HttpErrorKind::Status
                    && r.error().http_status > 0) {
                    out.status = r.error().http_status;
                    return out;
                }
                out.status = 0;
                out.error  = r.error().render();
                return out;
            }

            // Manual redirect following (agentty's client doesn't auto-follow).
            if (r->status >= 300 && r->status < 400 && in.method != "HEAD") {
                std::string loc;
                for (const auto& h : r->headers)
                    if (h.name == "location") { loc = h.value; break; }
                if (!loc.empty()) {
                    visited.push_back(url);
                    auto nxt = resolve_redirect(p, loc);
                    if (nxt.empty()
                        || std::find(visited.begin(), visited.end(), nxt) != visited.end()) {
                        out.status = r->status;   // give the tool what we have
                        out.body   = std::move(r->body);
                        for (const auto& h : r->headers) out.headers.push_back({h.name, h.value});
                        return out;
                    }
                    url = std::move(nxt);
                    continue;
                }
            }

            out.status = r->status;
            out.body   = std::move(r->body);
            for (const auto& h : r->headers) out.headers.push_back({h.name, h.value});
            return out;
        }
        out.status = 0;
        out.error  = "too many redirects";
        return out;
    }
};

// ── Result → ExecResult decode ─────────────────────────────────────────
//   Map an mcp cap::Result back into agentty's ExecResult. On error wrap
//   the text in a ToolError; on success carry text + (decoded) FileChange.
//   mcp's FileChange has no hunks (host recomputes), so feed before/after
//   through diff::compute to rebuild the full FileChange the diff-review
//   UI consumes.

ExecResult decode_result(const std::string& tool_name, ::mcp::cap::Result r) {
    if (r.is_error)
        return std::unexpected(ToolError::unknown(std::move(r.text)));

    ToolOutput out;
    out.text = std::move(r.text);

    if (auto ch = mt::read_change(r); ch.has_value()) {
        // ch carries path/added/removed/before/after but no hunks; rebuild
        // the structured hunks agentty's diff-review needs.
        FileChange fc = diff::compute(ch->path, ch->before, ch->after);
        // diff::compute recomputes added/removed identically; keep its
        // structured result (authoritative) but trust the provider's
        // before/after verbatim.
        fc.original_contents = ch->before;
        fc.new_contents      = ch->after;
        out.change = std::move(fc);
    }
    (void)tool_name;
    return out;
}

// Process-lifetime keep-alive for the provider: the ToolDef::execute
// closures capture a shared_ptr to it, but we also park it here so its
// HostServices adapters (incl. the HttpClient) outlive every closure.
struct ProviderKeepAlive {
    std::shared_ptr<::mcp::cap::CapabilityProvider> provider;
    std::shared_ptr<mt::HttpClient>                 http;
};
ProviderKeepAlive& keep_alive() { static ProviderKeepAlive k; return k; }

} // namespace

std::vector<ToolDef> build_mcp_tool_defs() {
    auto& ka = keep_alive();
    ka.http = std::make_shared<AgenttyHttpClient>();

    mt::HostServices svc;
    svc.http = ka.http;
    // The 5 host-coupled backends (memory/todo/skill/retriever/subagent)
    // are injected in a follow-up; until then those tools continue to be
    // served by agentty's native factories and are NOT advertised here.

    mt::ToolsetConfig cfg;   // all Tier-1 families on by default
    auto provider = mt::make_provider(svc, cfg, "local");
    ka.provider = provider;

    std::vector<ToolDef> defs;
    for (const auto& spec : provider->list()) {
        ToolDef def;
        def.name        = ToolName{spec.name};
        def.description  = spec.description.has_value() ? *spec.description : std::string{};
        def.input_schema = ::mcp::to_json(spec.inputSchema);
        def.effects      = EffectSet{mt::effects_for_builtin(spec.name).bits()};
        // eager_input_streaming: write/edit/bash/git_commit benefit from
        // token-by-token tool-input streaming (multi-KB bodies).
        const std::string& n = spec.name;
        def.eager_input_streaming =
            (n == "write" || n == "edit" || n == "bash" || n == "git_commit");

        std::string tool_name = spec.name;
        def.execute = [provider, tool_name](const nlohmann::json& args) -> ExecResult {
            auto r = provider->execute(::mcp::cap::Request{tool_name, args});
            return decode_result(tool_name, std::move(r));
        };
        defs.push_back(std::move(def));
    }
    return defs;
}

} // namespace agentty::tools
