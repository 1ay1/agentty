// agentty::provider — ExternalAcpBackend → Provider adapter.
// See acp_provider_adapter.hpp.

#include "agentty/provider/acp_provider_adapter.hpp"

#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

#include <acp/acp.hpp>          // acp::InitializeParams, SessionUpdate arms, match
#include <nlohmann/json.hpp>

#include "agentty/io/http.hpp"                       // http::CancelToken
#include "agentty/provider/acp_agents.hpp"           // resolve_acp_agent
#include "agentty/provider/acp_backend.hpp"          // TurnSink, TurnResult
#include "agentty/provider/external_acp_backend.hpp" // ExternalAcpBackend, spawn_acp_agent
#include "agentty/provider/stream_epilogue.hpp"        // StreamResult
#include "agentty/runtime/msg.hpp"                   // Stream* Msgs
#include "agentty/tool/util/fs_helpers.hpp"          // workspace_root (for cwd)

namespace agentty::provider {

namespace {

namespace a = acp;
using json = nlohmann::json;

// One spawned agent + the backend that drives it. Cached per agent id: the
// subprocess is long-lived, its ACP session reused across requests.
struct LiveAgent {
    std::unique_ptr<ExternalAcpBackend> backend;
    SpawnedAcpAgent                     agent;   // owns the subprocess + transport
};

std::mutex                                             g_cache_mu;
std::unordered_map<std::string, std::shared_ptr<LiveAgent>> g_cache;

// Extract the plain text of a ContentBlock (text + text-resource arms only;
// other arms contribute nothing to a streamed delta).
std::string block_text(const a::ContentBlock& b) {
    std::string out;
    a::match(b,
        [&](const a::TextContent& t) { out += t.text; },
        [&](const a::ResourceContent& r) {
            a::match(r.resource,
                [&](const a::TextResource& tr) { out += tr.text; },
                [&](const a::BlobResource&)    {});
        },
        [&](const auto&) {});
    return out;
}

// Build the client's InitializeParams. We advertise the fs + terminal client
// capabilities so a spec-conformant agent knows it may call back into us
// (our make_handlers() wires those). acp-cpp doesn't enforce this agent-side,
// but real agents check it.
a::InitializeParams make_init() {
    a::InitializeParams init;
    init.clientCapabilities.fs.readTextFile  = true;
    init.clientCapabilities.fs.writeTextFile = true;
    init.clientCapabilities.terminal         = true;
    return init;
}

// Get (or lazily spawn) the live agent for `agent_id`. Returns nullptr and
// fills `err` on any launch failure.
std::shared_ptr<LiveAgent> acquire(const std::string& agent_id, std::string& err) {
    {
        std::lock_guard<std::mutex> lk(g_cache_mu);
        if (auto it = g_cache.find(agent_id); it != g_cache.end())
            return it->second;
    }

    auto spec = resolve_acp_agent(agent_id);
    if (!spec) {
        err = "unknown ACP agent '" + agent_id +
              "' (no built-in default and no .agentty/acp-agents.json entry)";
        return nullptr;
    }

    ExternalAcpOptions opts;
    opts.cwd           = spec->cwd.empty()
                       ? tools::util::workspace_root().string()
                       : spec->cwd;
    opts.reuse_session = true;
    opts.delegate      = default_sandbox_delegate();

    auto live     = std::make_shared<LiveAgent>();
    live->backend = std::make_unique<ExternalAcpBackend>(std::move(opts));

    std::string spawn_err;
    live->agent = spawn_acp_agent(spec->argv(), make_init(),
                                  live->backend->make_handlers(), spawn_err);
    if (!live->agent.ok()) {
        err = "failed to launch ACP agent '" + agent_id + "': " + spawn_err;
        return nullptr;
    }
    live->backend->connect(*live->agent.connection);

    std::lock_guard<std::mutex> lk(g_cache_mu);
    // Another thread may have raced us; keep the first winner.
    if (auto it = g_cache.find(agent_id); it != g_cache.end())
        return it->second;
    g_cache.emplace(agent_id, live);
    return live;
}

} // namespace

StreamResult stream_external_acp(const std::string& agent_id, Request req, EventSink sink) {
    sink(StreamStarted{});

    std::string err;
    auto live = acquire(agent_id, err);
    if (!live) {
        StreamError e; e.message = err;
        sink(std::move(e));
        return StreamResult::failed(err.empty() ? "ACP agent spawn failed" : err);
    }

    // Translate the round's SessionUpdates into agentty Stream* Msgs. The tool
    // arms map SU_ToolCall → Start+Delta(rawInput)+End (agentty's agent loop
    // executes the tool itself; the delta carries the args JSON).
    TurnSink tsink;
    tsink.update = [&sink](a::SessionUpdate su) {
        a::match(su,
            [&](const a::SU_AgentMessageChunk& c) {
                std::string t = block_text(c.content);
                if (!t.empty()) sink(StreamTextDelta{std::move(t)});
            },
            [&](const a::SU_AgentThoughtChunk& c) {
                std::string t = block_text(c.content);
                if (!t.empty()) sink(StreamThinkingDelta{std::move(t), {}});
            },
            [&](const a::SU_ToolCall& tc) {
                const auto& call = tc.toolCall;
                sink(StreamToolUseStart{
                    ToolCallId{call.toolCallId.value},
                    ToolName{call.title.empty() ? call.toolCallId.value : call.title}});
                if (call.rawInput.has_value() && !call.rawInput.value().is_null())
                    sink(StreamToolUseDelta{call.rawInput.value().dump()});
                sink(StreamToolUseEnd{});
            },
            [&](const a::SU_Usage& u) {
                StreamUsage su2;
                // ACP reports a single aggregate `used`; map it onto the input
                // token field so the context-window meter advances. `size` is
                // the agent's advertised window if it sent one.
                su2.input_tokens = static_cast<int>(u.used < 0 ? 0 : u.used);
                sink(std::move(su2));
            },
            [&](const auto&) { /* plan / commands / mode / info: not streamed */ });
    };

    auto cancel = std::make_shared<http::CancelToken>();
    req.cancel  = cancel;   // the runtime trips this on Esc (see below)

    // ACP's per-round TurnResult (acp_backend.hpp) is the ADJACENT layer's
    // outcome; fold it onto the shared provider StreamResult so the ACP arm
    // reports through the same value type as the native transports.
    TurnResult res = live->backend->prompt(req, tsink, cancel);

    if (!res.ok()) {
        if (res.error && res.error->user_cancel) {
            // A user cancel is a clean stop, not an error surface.
            sink(StreamFinished{StopReason::EndTurn});
            StreamResult sr;
            sr.end  = StreamEnd::UserCancelled;
            sr.stop = StopReason::EndTurn;
            sr.error = std::string{"cancelled"};
            return sr;
        }
        StreamError e;
        e.message = res.error ? res.error->message : "ACP agent error";
        if (res.error) e.retry_after = res.error->retry_after;
        StreamResult sr;
        sr.end         = StreamEnd::TransportError;
        sr.stop        = res.stop;
        sr.error       = e.message;
        sr.retry_after = e.retry_after;
        sink(std::move(e));
        return sr;
    }
    sink(StreamFinished{res.stop});
    StreamResult sr;
    sr.end  = StreamEnd::CleanClose;
    sr.stop = res.stop;
    return sr;
}

void release_acp_agents() noexcept {
    std::unordered_map<std::string, std::shared_ptr<LiveAgent>> drained;
    {
        std::lock_guard<std::mutex> lk(g_cache_mu);
        drained.swap(g_cache);
    }
    // Destroy outside the lock: each LiveAgent dtor runs ExternalAcpBackend's
    // hardened teardown watchdog (bounded even for a wedged agent).
    for (auto& [id, live] : drained) {
        if (!live) continue;
        // Order: backend (holds non-owning conn_) → connection → process.
        live->backend.reset();
        live->agent.connection.reset();
        live->agent.process.reset();
    }
}

} // namespace agentty::provider
