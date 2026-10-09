// mcp_tools_bridge.cpp — see mcp_tools_bridge.hpp.
//
// Adapts mcp-cpp's make_provider() toolset into agentty ToolDefs. The
// provider owns each Tier-1 tool's implementation; this file owns the
// thin re-wrap: build a ToolDef per advertised tool, dispatch execute()
// into the provider, and decode the `_mcp_tools` meta (effects +
// FileChange) back into agentty's ToolOutput.

#include "agentty/tool/mcp_tools_bridge.hpp"
#include "agentty/tool/util/exec.hpp"   // the host's one way to run a program
#include "agentty/tool/mcp_tools_backends.hpp"

#include "agentty/diff/diff.hpp"
#include "agentty/io/http.hpp"
#include "agentty/rag/rag_adapter.hpp"   // rag::feedback::note_file_opened (learning loop)
#include "agentty/tool/registry.hpp"   // tools::progress::emit
#include "agentty/runtime/app/update/stream_args.hpp"  // canonify_tool_args (`cmd`→`command`)
#include "agentty/tool/spec.hpp"       // spec catalog — effects authority
#include "agentty/tool/util/fs_helpers.hpp"   // agentty workspace_root()

#include <mcp/tools/toolset.hpp>
#include <mcp/tools/host.hpp>
#include <maya/runtime.hpp>

#include <cstdlib>   // std::getenv
#include <mcp/tools/meta.hpp>
#include <mcp/tools/util/fs_helpers.hpp>
#include "agentty/tool/util/sandbox.hpp"
#include "agentty/tool/util/handoff_gate.hpp"
#include <mcp/cap/capability.hpp>
#include <mcp/cap/local.hpp>
#include <mcp/codec.hpp>

#include <algorithm>
#include <array>
#include <cctype>    // std::isalnum — env-name validation in the sandbox hook
#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace agentty::tools::tool_state {
// See GuardedToolState below.
struct FnRef { const std::function<void(::mcp::tools::ToolState&)>* fn; };
}  // namespace agentty::tools::tool_state
MAYA_SENDABLE(agentty::tools::tool_state::FnRef);
MAYA_SENDABLE(mcp::tools::ToolState);

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

        // IPv4-mapped / -compatible IPv6 (::ffff:127.0.0.1, ::ffff:a9fe:a9fe,
        // ::127.0.0.1) resolve to the embedded IPv4 and are a classic SSRF
        // filter bypass: the prefix checks above miss them and the dotted-quad
        // parser below rejects them (they contain ':'). If an IPv6 literal
        // carries a trailing IPv4 (dotted, or the last two hex groups of a
        // ::ffff: mapping), fold it to dotted form and let the IPv4 range
        // check below decide. A genuine public IPv6 (2606:4700::1111) has no
        // such embedded v4 and is left to pass — we don't blanket-block v6.
        if (h.find(':') != std::string::npos
            && (h.starts_with("::ffff:") || h.starts_with("::"))) {
            auto last_colon = h.rfind(':');
            std::string tail = h.substr(last_colon + 1);
            if (tail.find('.') != std::string::npos) {
                h = tail;   // ::ffff:127.0.0.1 → "127.0.0.1"
            } else {
                // ::ffff:7f00:0001 — last two hex groups encode the v4.
                auto prev_colon = (last_colon == 0) ? std::string::npos
                                                    : h.rfind(':', last_colon - 1);
                std::string g_hi = (prev_colon != std::string::npos)
                                       ? h.substr(prev_colon + 1, last_colon - prev_colon - 1)
                                       : std::string{};
                auto hex4 = [](const std::string& g, unsigned& v) -> bool {
                    if (g.empty() || g.size() > 4) return false;
                    v = 0;
                    for (char ch : g) {
                        unsigned d;
                        if (ch >= '0' && ch <= '9') d = static_cast<unsigned>(ch - '0');
                        else if (ch >= 'a' && ch <= 'f') d = static_cast<unsigned>(ch - 'a' + 10);
                        else return false;
                        v = v * 16 + d;
                    }
                    return true;
                };
                unsigned hi, lo;
                if (h.starts_with("::ffff:") && !g_hi.empty()
                    && hex4(g_hi, hi) && hex4(tail, lo)) {
                    h = std::to_string((hi >> 8) & 0xff) + "." + std::to_string(hi & 0xff)
                      + "." + std::to_string((lo >> 8) & 0xff) + "." + std::to_string(lo & 0xff);
                }
            }
        }

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
            // Defense in depth: host_blocked() above rejects obvious internal
            // NAMES/literals fast, but a public hostname can DNS-rebind to an
            // internal IP that the name check can't see. ssrf_guard re-checks
            // the RESOLVED sockaddr at dial time and refuses loopback/private/
            // link-local/metadata targets — the authoritative backstop.
            req.ssrf_guard = true;
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

    // Strip mcp-cpp's output-budget truncation marker before it reaches the
    // UI — but ONLY for file-mutating tools, where it's both noise and safe to
    // drop. The dispatcher (apply_output_budget) appends a line like
    //   "[... N chars elided — output exceeded tool's budget; refine your
    //    request to see more ...]"
    // to bound the WIRE payload. For edit/write/apply_patch/replace the text
    // is a human-readable diff/confirmation AND the COMPLETE change is carried
    // structurally (read_changes below, before/after), so the marker is
    // redundant for the model and actively wrong in the review card (it was
    // rendering a grey "refine your request" line under the diff). For
    // read/grep/bash the marker is left intact — there the model genuinely
    // needs to know output was clipped so it can page/refine.
    {
        const bool mutating =
            tool_name == "edit" || tool_name == "write" ||
            tool_name == "apply_patch" || tool_name == "replace";
        if (mutating) {
            if (auto m = out.text.rfind("[... "); m != std::string::npos) {
                const auto tail = std::string_view{out.text}.substr(m);
                if (tail.find("chars elided") != std::string_view::npos &&
                    tail.find("...]") != std::string_view::npos) {
                    while (m > 0 &&
                           (out.text[m - 1] == '\n' || out.text[m - 1] == ' '))
                        --m;
                    out.text.resize(m);
                }
            }
        }
    }

    // Decode EVERY file the tool changed — the single-file `change` (edit /
    // write / apply_patch) and the multi-file `changes` array (replace). Each
    // carries path/before/after but no hunks; rebuild the structured hunks the
    // diff-review needs. All land in out.changes; out.change stays the
    // first for any legacy single-file consumer.
    auto raw = mt::read_changes(r);
    for (const auto& ch : raw) {
        FileChange fc = diff::compute(ch.path, ch.before, ch.after);
        fc.original_contents = ch.before;
        fc.new_contents      = ch.after;
        out.changes.push_back(std::move(fc));
    }
    if (!out.changes.empty()) out.change = out.changes.front();

    // Images the tool surfaced (read on an image file): decode the base64 the
    // tool body stashed in structured meta back to raw bytes and map onto the
    // host ImageContent. These become image blocks in this call's tool_result
    // for a vision model; a non-vision model just sees the [image ...] note in
    // out.text, so nothing here needs a capability check — the wire layer drops
    // them for models that can't see (see the vision gate at send time).
    for (auto& im : mt::read_images(r))
        out.images.push_back(ImageContent{std::move(im.media_type),
                                          std::move(im.bytes)});

    (void)tool_name;
    return out;
}

// The mcp-cpp tools' memory between calls (read cache, file snapshots,
// background processes, repo graphs), under one maya::guarded. The workspace
// root and read roots are agentty's (main sets one, the skills scanner adds
// the other); they are copied in on every access, so there is one source of
// truth and nothing to mirror.
//
// The tools hand us a function to run on the state. guarded<T> refuses an
// opaque callable as an argument, because it could take a second lock while
// this one is held. These can't: mcp-cpp holds no locks and starts nothing
// (its concurrency allowlist is empty), and every with() body in it only
// reads and writes ToolState. So the one callable type is opted in here.
using StateFn    = std::function<void(mt::ToolState&)>;
using StateFnRef = tool_state::FnRef;

class GuardedToolState final : public mt::StateAccess {
  public:
    void with(const StateFn& f) override {
        auto ws    = util::workspace_root();
        auto roots = util::read_roots_now();
        st_.with([](mt::ToolState& s, StateFnRef fn,
                    std::filesystem::path w, std::vector<std::filesystem::path> r) {
            s.workspace_root = std::move(w);
            s.read_roots     = std::move(r);
            (*fn.fn)(s);
        }, StateFnRef{&f}, std::move(ws), std::move(roots));
    }
  private:
    maya::guarded<mt::ToolState> st_;
};

// Process-lifetime keep-alive for the provider: the ToolDef::execute
// closures capture a shared_ptr to it, but we also park it here so its
// HostServices adapters (incl. the HttpClient) outlive every closure.
struct ProviderKeepAlive {
    std::shared_ptr<::mcp::cap::CapabilityProvider> provider;
    std::shared_ptr<mt::HttpClient>                 http;
    std::shared_ptr<mt::Exec>                       exec;
    std::shared_ptr<mt::Executor>                   executor;
    std::shared_ptr<mt::StateAccess>                state;
};
ProviderKeepAlive& keep_alive() { static ProviderKeepAlive k; return k; }

} // namespace

std::vector<ToolDef> build_mcp_tool_defs() {
    auto& ka = keep_alive();
    ka.http = std::make_shared<AgenttyHttpClient>();
    // Running another program is a capability the HOST supplies, exactly
    // like the HttpClient above: mcp-cpp states what it wants and we do it,
    // over jaal's process + reactor. One implementation, in one place, with
    // the idle-vs-wall policy that the two old poll loops disagreed about.
    ka.exec = util::make_exec();
    // Parallel scans too: mcp-cpp owns no threads, so its grep / structural
    // search / repo map fan out on agentty's executor (maya::scope).
    ka.executor = util::make_executor();
    ka.state = std::make_shared<GuardedToolState>();

    mt::HostServices svc;
    svc.http = ka.http;
    svc.exec = ka.exec;
    svc.executor = ka.executor;
    svc.state    = ka.state;
    // Inject the host-coupled backends (memory/skill/retriever/subagent).
    // todo stays null — its shell renders identical text with no host state.
    install_host_backends(svc);

    mt::ToolsetConfig cfg;   // all Tier-1 families on by default
    // AGENTTY_NO_TRANSFORMS=1 sheds the transform/aggregate/structured-data
    // family (extract/aggregate/replace/read_filter/json_query) — ~5 KB of
    // schema off every request — for a minimal or latency-sensitive profile,
    // WITHOUT losing grep/read/edit. Mirrors AGENTTY_NO_HOOKS's convention.
    if (const char* v = std::getenv("AGENTTY_NO_TRANSFORMS");
        v && v[0] && v[0] != '0' && v[0] != 'f' && v[0] != 'F' && v[0] != 'n' && v[0] != 'N')
        cfg.transforms = false;
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
        def.execute = [provider, tool_name](const nlohmann::json& args_in) -> ExecResult {
            // Canonify alias keys HERE, at the dispatch boundary.
            //
            // PR #66 canonified at every stream-parse site, which fixes the
            // reported bug (a `cmd`-shaped call failing the required-field
            // guard) but leaves the property depending on where the args came
            // from. Any path that does not pass through those parsers --
            // a replayed thread, a tool call injected by the ACP client, a
            // future transport -- reaches this lambda with `cmd` set and
            // `command` absent, and mcp-cpp's ArgReader has NO alias table
            // (it is str(key)/require_str(key), nothing more), so the
            // executor reads an empty command and runs it.
            //
            // Doing it at the single point every path funnels through makes
            // the invariant "the executor always sees `command`" hold by
            // construction rather than by having remembered to call it at N
            // call sites. The parse-site calls stay: the permission card, the
            // live output panel and the shell-detour check all read
            // tc.args["command"] BEFORE dispatch, so they need the canonical
            // key too. This is the backstop, not a replacement.
            nlohmann::json args = args_in;
            (void)::agentty::app::detail::canonify_tool_args(tool_name, args);

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
            // TRUST HANDOFF GATE. Before the bytes land, not after.
            //
            // This is the enforcement half of the Sandbox pane's "Agent writes
            // host-executed files" row, which until now persisted a policy
            // that nothing read. It sits here rather than inside the sandbox
            // because the sandbox is the wrong altitude: writing
            // .vscode/tasks.json is an ordinary write to a path the agent is
            // allowed to write, and the escape happens later, outside, when
            // VS Code runs it. No mount namespace sees that.
            //
            // All four authoring tools, not just `write`: `edit` can add a
            // credential helper to an existing .git/config, `apply_patch` can
            // do it in a diff, and `move` can walk an inert file into a
            // trusted name -- which is the Cursor 3.0.0 bug (a .git directory
            // under another name) in reverse. Gating only `write` would leave
            // three doors open and read as covered.
            if (tool_name == "write" || tool_name == "edit" ||
                tool_name == "apply_patch" || tool_name == "move") {
                // `move` names its target `destination`; the rest use `path`
                // or `file_path`. Checking the DESTINATION is the point for a
                // move -- the source being inert is exactly the attack.
                for (const char* key : {"destination", "path", "file_path"}) {
                    auto it = args.find(key);
                    if (it == args.end() || !it->is_string()) continue;
                    auto v = tools::util::handoff::check(
                        it->get_ref<const std::string&>(), tool_name);
                    if (!v.allowed)
                        return std::unexpected(
                            ToolError::denied(std::move(v.reason)));
                    break;   // one path per call; first key present wins
                }
            }

            // SHELL-PATH DETECTION. The gate above needs a path argument, and
            // `shell` has none -- it hands over an opaque string, which is how
            // `cat > .vscode/tasks.json` walked past a policy that correctly
            // knew the path was dangerous.
            //
            // Parsing the command was rejected (grammar arms race; see
            // docs/design/shell-write-gate.md and the long note in
            // handoff_gate.hpp). So instead: observe the FILESYSTEM. Snapshot
            // the trusted-shape paths, run the call, compare. No quoting trick
            // evades it because it never looks at the command.
            //
            // Detection, not prevention: by the time we can compare, the bytes
            // are written. Refusing afterwards would be theatre -- the honest
            // outcome is a loud, attributable event on the feed the Sandbox
            // pane already renders. A read-only bind is the prevention half
            // and needs a mount namespace, so it cannot be the only answer on
            // the hardened hosts claybin exists to serve.
            const bool watchable =
                tool_name == "shell" || tool_name == "process_start" ||
                tool_name == "test" || tool_name == "diagnostics";
            tools::util::handoff::TrustedSnapshot before;
            if (watchable)
                before = tools::util::handoff::snapshot_trusted(
                    tools::util::workspace_root().string());

            ::mcp::cap::Request req{tool_name, args};
            req.cancelled = [] { return tools::cancellation::requested(); };
            req.reader    = tools::reader::current();
            auto r = provider->execute(req);

            if (watchable && !before.empty()) {
                // Name the command, not the tool: "shell" tells a user nothing
                // about what happened. This is the one place the opaque string
                // earns its keep -- explaining after the fact, never deciding.
                std::string who = tool_name;
                if (auto c = args.find("command");
                    c != args.end() && c->is_string())
                    who = c->get_ref<const std::string&>();
                (void)tools::util::handoff::review_trusted(before, who);
            }

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
        "read", "edit", "apply_patch", "write", "move", "remove", "shell",
        "process_start", "process_poll", "process_stop",
        "grep", "glob", "list_dir",
        "search_structural",
        "extract", "aggregate", "read_filter", "replace",
        "json_query",
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

} // namespace agentty::tools
