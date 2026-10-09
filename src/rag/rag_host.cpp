// src/rag/rag_host.cpp — rag-cpp's host services on agentty. See rag_host.hpp.

#include "agentty/rag/rag_host.hpp"

#include <chrono>
#include <memory>
#include <string>
#include <variant>

#include <maya/runtime.hpp>
#include <rag/bridge/process.hpp>

#include "agentty/io/http.hpp"
#include "agentty/tool/util/exec.hpp"
#include "agentty/util/child_process.hpp"

namespace agentty::rag {

namespace {

namespace rd = ::rag::dense;

// rag's POST seam over agentty's HTTP client. These are endpoints the user
// configured (a local Ollama, a hosted API), so plaintext is allowed for
// non-TLS and the SSRF guard (meant for model-chosen URLs) is off.
class AgenttyHttp final : public rd::HttpTransport {
public:
    ::rag::Result<rd::HttpResponse> post(const rd::HttpRequest& r) const override {
        http::Request req;
        req.method    = http::HttpMethod::Post;
        req.host      = std::string(r.host);
        req.port      = r.port;
        req.path      = std::string(r.path);
        req.body      = std::string(r.body);
        req.plaintext = !r.tls;
        req.headers.push_back({"content-type", "application/json"});
        for (const auto& [k, v] : r.headers) req.headers.push_back({k, v});
        http::Timeouts t;
        t.connect = std::min(r.timeout, std::chrono::milliseconds{10'000});
        t.total   = r.timeout;
        auto res = http::default_client().send(req, t);
        if (!res) {
            if (res.error().kind == http::HttpErrorKind::Status)
                return rd::HttpResponse{res.error().http_status, res.error().detail};
            return ::rag::fail<rd::HttpResponse>(::rag::Errc::transport_error, res.error().render());
        }
        return rd::HttpResponse{res->status, std::move(res->body)};
    }
};

// A bridge peer as a child process: its stdout is read, its stdin written.
::rag::Result<std::shared_ptr<::rag::bridge::Channel>> spawn_peer(const ::rag::bridge::ProcessSpec& spec) {
    util::ChildProcess::Spawn sp;
    sp.command = spec.argv.front();
    sp.args.assign(spec.argv.begin() + 1, spec.argv.end());
    sp.env_kv = spec.env;
    if (!spec.cwd.empty()) sp.cwd = spec.cwd;
    std::shared_ptr<util::ChildProcess> child;
    try {
        child = std::make_shared<util::ChildProcess>(sp);
    } catch (const std::exception& e) {
        return ::rag::fail<std::shared_ptr<::rag::bridge::Channel>>(
            ::rag::Errc::unavailable, std::string("cannot start ") + spec.argv.front() + ": " + e.what());
    }
    ::rag::bridge::LineIo io;
    io.read = [c = child.get()](char* buf, std::size_t cap) -> std::size_t {
        auto* sb = c->out().rdbuf();
        if (!sb || cap == 0) return 0;
        const auto ch = sb->sbumpc();
        if (ch == std::char_traits<char>::eof()) return 0;
        buf[0] = static_cast<char>(ch);
        std::size_t n = 1;
        while (n < cap && sb->in_avail() > 0) {
            const auto more = sb->sgetn(buf + n, std::min<std::streamsize>(
                sb->in_avail(), static_cast<std::streamsize>(cap - n)));
            if (more <= 0) break;
            n += static_cast<std::size_t>(more);
        }
        return n;
    };
    io.write = [c = child.get()](std::string_view bytes) {
        c->in().write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        c->in().flush();
        return c->in().good();
    };
    io.keep = child;
    return std::shared_ptr<::rag::bridge::Channel>(
        std::make_shared<::rag::bridge::LineChannel>(std::move(io), spec.name));
}

}  // namespace

::rag::util::Splitter splitter() {
    return ::rag::util::Splitter(::agentty::tools::util::scope_fan_out,
                                 ::agentty::tools::util::fan_out_width());
}

::rag::plugin::HostIo host_io() {
    ::rag::plugin::HostIo io;
    io.http  = std::make_shared<AgenttyHttp>();
    io.spawn = spawn_peer;
    return io;
}

const ::rag::plugin::Registries& registries() {
    static const ::rag::plugin::Registries regs = ::rag::plugin::Registries::with_io(host_io());
    return regs;
}

::rag::loaders::RunFn converter_runner() {
    return [exec = tools::util::make_exec()](const std::vector<std::string>& argv)
               -> std::optional<std::string> {
        namespace mt = ::mcp::tools;
        mt::ExecRequest req;
        req.program          = {argv.front(), {argv.begin() + 1, argv.end()}};
        req.budgets.idle     = std::chrono::seconds{60};
        req.budgets.wall     = std::chrono::seconds{300};
        req.max_output_bytes = 64u << 20;
        auto r = exec->run(req);
        if (std::holds_alternative<mt::StartFailed>(r.outcome)) return std::nullopt;
        return std::move(r.output);
    };
}

}  // namespace agentty::rag
