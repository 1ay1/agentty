// mcp_tools_bridge.cpp — see mcp_tools_bridge.hpp.
//
// Adapts mcp-cpp's make_provider() toolset into agentty ToolDefs. The
// provider owns each Tier-1 tool's implementation; this file owns the
// thin re-wrap: build a ToolDef per advertised tool, dispatch execute()
// into the provider, and decode the `_mcp_tools` meta (effects +
// FileChange) back into agentty's ToolOutput.

#include "agentty/tool/mcp_tools_bridge.hpp"
#include "agentty/tool/mcp_tools_backends.hpp"

#include "agentty/diff/diff.hpp"
#include "agentty/io/http.hpp"
#include "agentty/rag/rag_adapter.hpp"   // rag::feedback::note_file_opened (learning loop)
#include "agentty/tool/registry.hpp"   // tools::progress::emit
#include "agentty/tool/spec.hpp"       // spec catalog — effects authority
#include "agentty/tool/util/fs_helpers.hpp"   // agentty workspace_root()

#include <mcp/tools/toolset.hpp>
#include <mcp/tools/host.hpp>
#include <mcp/tools/meta.hpp>
#include <mcp/tools/util/fs_helpers.hpp>
#include <mcp/tools/util/progress.hpp>
#include <mcp/tools/util/sandbox.hpp>
#include <mcp/cap/capability.hpp>
#include <mcp/cap/local.hpp>
#include <mcp/codec.hpp>

#include <algorithm>
#include <array>
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

    // SSRF guard applied to EVERY hop (initial URL AND each redirect target).
    // mcp-cpp's web shell validates only the first URL it's handed; because
    // this adapter follows 3xx itself, a public URL that redirects to
    // 169.254.169.254 (cloud metadata), 127.0.0.1, or an RFC1918 host would
    // otherwise sail straight through. Mirrors mcp's is_blocked_host: folds
    // the many legal IPv4 spellings (decimal/hex/octal/short-form) to a
    // dotted quad before range-checking, plus loopback/link-local/ULA IPv6.
    static bool host_blocked(std::string_view host) {
        if (host.size() >= 2 && host.front() == '[' && host.back() == ']')
            host = host.substr(1, host.size() - 2);
        std::string h{host};
        for (char& c : h) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (h.empty()) return true;
        if (h == "localhost" || h.ends_with(".localhost")) return true;
        if (h == "metadata" || h == "metadata.google.internal") return true;
        if (h == "0") return true;
        if (h == "::1" || h == "::") return true;
        if (h.starts_with("fc") || h.starts_with("fd")) return true;   // ULA
        if (h.starts_with("fe8") || h.starts_with("fe9")
            || h.starts_with("fea") || h.starts_with("feb")) return true; // link-local

        // Fold the IPv4 spellings inet_aton/getaddrinfo accept into one u32.
        auto parse_part = [](std::string_view p, unsigned long& v) -> bool {
            if (p.empty()) return false;
            int base = 10;
            if (p.size() >= 2 && p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
                base = 16; p.remove_prefix(2);
                if (p.empty()) return false;
            } else if (p.size() >= 2 && p[0] == '0') { base = 8; p.remove_prefix(1); }
            v = 0;
            for (char ch : p) {
                unsigned dig;
                if (ch >= '0' && ch <= '9') dig = static_cast<unsigned>(ch - '0');
                else if (base == 16 && ch >= 'a' && ch <= 'f') dig = static_cast<unsigned>(ch - 'a' + 10);
                else return false;
                if (dig >= static_cast<unsigned>(base)) return false;
                v = v * static_cast<unsigned long>(base) + dig;
                if (v > 0xffffffffUL) return false;
            }
            return true;
        };
        std::vector<unsigned long> parts;
        std::size_t start = 0;
        bool numeric = true;
        for (std::size_t i = 0; i <= h.size() && numeric; ++i) {
            if (i == h.size() || h[i] == '.') {
                unsigned long v;
                if (!parse_part(std::string_view{h}.substr(start, i - start), v)) { numeric = false; break; }
                parts.push_back(v);
                start = i + 1;
            }
        }
        if (numeric && !parts.empty() && parts.size() <= 4) {
            std::uint32_t ip = 0;
            bool ok = true;
            switch (parts.size()) {
                case 1: ip = static_cast<std::uint32_t>(parts[0]); break;
                case 2: if (parts[0] > 0xff || parts[1] > 0xffffff) { ok = false; break; }
                        ip = static_cast<std::uint32_t>((parts[0] << 24) | parts[1]); break;
                case 3: if (parts[0] > 0xff || parts[1] > 0xff || parts[2] > 0xffff) { ok = false; break; }
                        ip = static_cast<std::uint32_t>((parts[0] << 24) | (parts[1] << 16) | parts[2]); break;
                case 4: if (parts[0] > 0xff || parts[1] > 0xff || parts[2] > 0xff || parts[3] > 0xff) { ok = false; break; }
                        ip = static_cast<std::uint32_t>((parts[0] << 24) | (parts[1] << 16) | (parts[2] << 8) | parts[3]); break;
            }
            if (ok) {
                unsigned a = (ip >> 24) & 0xff, b = (ip >> 16) & 0xff;
                if (a == 127 || a == 0 || a == 10) return true;
                if (a == 169 && b == 254) return true;
                if (a == 172 && b >= 16 && b <= 31) return true;
                if (a == 192 && b == 168) return true;
                if (a == 100 && b >= 64 && b <= 127) return true;
                if (a >= 224) return true;
                return false;   // numeric but public
            }
        }
        return false;
    }

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
            try {
                int port_int = std::stoi(std::string{authority.substr(colon + 1)});
                if (port_int <= 0 || port_int > 65535) return out;   // bad port → not ok
                out.port = static_cast<uint16_t>(port_int);
            }
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
            if (host_blocked(p.host)) {
                out.status = 0;
                out.error  = "blocked host (SSRF guard): " + p.host;
                return out;
            }

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

ToolError decode_tool_error(std::string text) {
    using K = ErrorKind;
    static constexpr std::array kinds = {
        std::pair{"invalid args", K::InvalidArgs},
        std::pair{"not found", K::NotFound},
        std::pair{"not a file", K::NotAFile},
        std::pair{"not a directory", K::NotADirectory},
        std::pair{"too large", K::TooLarge},
        std::pair{"binary", K::Binary},
        std::pair{"ambiguous", K::Ambiguous},
        std::pair{"no match", K::NoMatch},
        std::pair{"invalid regex", K::InvalidRegex},
        std::pair{"network", K::Network},
        std::pair{"spawn failed", K::Spawn},
        std::pair{"subprocess failed", K::Subprocess},
        std::pair{"io", K::Io},
        std::pair{"out of workspace", K::OutOfWorkspace},
        std::pair{"unknown", K::Unknown},
    };
    for (const auto& [label, kind] : kinds) {
        const std::string prefix = "[" + std::string{label} + "] ";
        if (text.starts_with(prefix))
            return ToolError{kind, text.substr(prefix.size())};
    }
    return ToolError::unknown(std::move(text));
}

ExecResult decode_result(const std::string& tool_name, ::mcp::cap::Result r) {
    if (r.is_error)
        return std::unexpected(decode_tool_error(std::move(r.text)));

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
    // Inject the host-coupled backends (memory/skill/retriever/subagent).
    // todo stays null — its shell renders identical text with no host state.
    install_host_backends(svc);

    mt::ToolsetConfig cfg;   // all Tier-1 families on by default
    auto provider = mt::make_provider(svc, cfg, "local");
    ka.provider = provider;

    std::vector<ToolDef> defs;
    for (const auto& spec : provider->list()) {
        ToolDef def;
        def.name        = ToolName{spec.name};
        def.description  = spec.description.has_value() ? *spec.description : std::string{};
        def.input_schema = ::mcp::to_json(spec.inputSchema);
        def.origin = ToolOrigin::Native;
        // Effects: agentty's spec catalog is the AUTHORITY for built-in
        // tools — the permission policy reads ToolDef::effects, the parallel
        // scheduler reads spec::lookup()->effects, and the catalog's
        // compile-time proofs (only_known_exec_tools, no_writefs_and_exec_
        // combo, readonly_invariants) are stated against it. mcp-cpp's
        // effects_for_builtin table had drifted (remember/forget/wipe were
        // "pure" there — no prompt despite writing memory.jsonl — and task
        // lacked Exec despite a subagent being able to run bash), which
        // split the gate from the scheduler. Prefer the catalog; the mcp
        // table only covers tools the catalog doesn't know.
        if (const auto* sp = tools::spec::lookup(spec.name)) {
            def.effects                = sp->effects;
            def.scheduling_effects     = tools::spec::sched_effects(*sp);
            def.eager_input_streaming  = sp->eager_input_streaming;
            def.max_output_chars       = sp->max_output_chars;
            def.timeout                = std::chrono::duration_cast<std::chrono::milliseconds>(sp->max_seconds);
            using NativeTrunc = tools::spec::ToolSpec::TruncStrategy;
            def.output_truncation = sp->trunc_strategy == NativeTrunc::Head
                ? OutputTruncation::Head
                : sp->trunc_strategy == NativeTrunc::Tail
                    ? OutputTruncation::Tail
                    : OutputTruncation::HeadTail;
        } else {
            def.effects = EffectSet{mt::effects_for_builtin(spec.name).bits()};
            def.scheduling_effects = def.effects;
        }

        std::string tool_name = spec.name;
        def.execute = [provider, tool_name](const nlohmann::json& args) -> ExecResult {
            // Bridge mcp's thread-local progress sink to agentty's on THIS
            // worker thread: cmd_factory already installed an agentty
            // progress::Scope here, so the subprocess runners inside the
            // mcp tools (bash/diagnostics/git) and the subagent loop stream
            // their live output straight to the parent tool card. RAII
            // scope clears it after the call so no stale sink leaks across
            // tool runs.
            ::mcp::tools::util::progress::Scope mcp_progress{
                [](std::string_view snap) { tools::progress::emit(snap); }};
            ::mcp::tools::util::cancellation::Scope mcp_cancellation{
                [] { return tools::cancellation::requested(); }};
            // LEARNING LOOP (win side): the agent ACTING on a file shortly
            // after search_docs surfaced a passage from it is the implicit
            // relevance signal — the passage pointed somewhere worth acting
            // on. `read` is the baseline signal; `edit`/`write` are STRONGER
            // (a confirmed mutation, not just a look), so credit those paths
            // too. Sub-microsecond no-op when nothing was recently surfaced.
            if (tool_name == "read" || tool_name == "edit" ||
                tool_name == "write") {
                if (auto it = args.find("path"); it != args.end() && it->is_string())
                    rag::feedback::note_file_opened(it->get_ref<const std::string&>());
                else if (auto fp = args.find("file_path");
                         fp != args.end() && fp->is_string())
                    rag::feedback::note_file_opened(fp->get_ref<const std::string&>());
            }
            auto r = provider->execute(::mcp::cap::Request{tool_name, args});
            return decode_result(tool_name, std::move(r));
        };
        defs.push_back(std::move(def));
    }

    // Reorder to agentty's recall-bias layout: the direct working tools
    // first (edit ahead of write — the cheapest nudge against whole-file
    // rewrites), the host-coupled tools (memory/task/skill/search_docs) last
    // so the model's strong first-listed bias stays on read/edit/bash. The
    // provider lists host-coupled shells first (registration order), so
    // without this the working tools sink to the bottom of the wire payload.
    static const std::vector<std::string_view> kOrder = {
        "read", "edit", "apply_patch", "write", "move", "remove", "bash",
        "process_start", "process_poll", "process_stop",
        "grep", "glob", "list_dir",
        "search_structural",
        "extract", "aggregate", "read_filter", "replace",
        "outline", "repo_map",
        "todo", "web_fetch", "web_search", "find_definition",
        "diagnostics", "test",
        "git_status", "git_diff", "git_log", "git_show", "git_blame", "git_commit",
        "git_branch", "git_stash", "git_rebase", "git_cherry_pick",
        "remember", "forget", "wipe_memory", "task", "skill", "search_docs",
        "search_code",
    };
    auto rank = [](std::string_view n) -> std::size_t {
        for (std::size_t i = 0; i < kOrder.size(); ++i)
            if (kOrder[i] == n) return i;
        return kOrder.size();   // unknown → after the known set, stable
    };
    std::stable_sort(defs.begin(), defs.end(),
        [&](const ToolDef& a, const ToolDef& b) {
            return rank(a.name.value) < rank(b.name.value);
        });
    return defs;
}

void wire_mcp_runtime(std::string_view sandbox_mode) {
    namespace mu = ::mcp::tools::util;

    // The workspace-root boundary is already mirrored automatically by
    // agentty's set_workspace_root() (it forwards into mcp's util layer in
    // the single canonical setter). Here we only need to mirror the sandbox
    // mode so bash/diagnostics/git run under the same bwrap / sandbox-exec
    // isolation. agentty's main.cpp already validated the flag + probed a
    // backend; translate the CLI string into mcp's Mode and let its sandbox
    // cache the probe result.
    auto mode = mu::sandbox::Mode::Auto;
    if      (sandbox_mode == "off") mode = mu::sandbox::Mode::Off;
    else if (sandbox_mode == "on")  mode = mu::sandbox::Mode::On;
    // "auto" / "" → Auto. main.cpp already rejected any other value.
    (void)mu::sandbox::init(mode);
}

} // namespace agentty::tools
