// src/mcp/connection.cpp — one MCP server as a capability provider. See
// connection.hpp.

#include "agentty/mcp/connection.hpp"

#include <mcp/mrtr.hpp>
#include <mcp/protocol.hpp>

#include "agentty/util/dbglog.hpp"
#include "agentty/util/sendable.hpp"

namespace agentty::mcp {

namespace m = ::mcp;
using Clock = std::chrono::steady_clock;

// The cached lists. The reader invalidates them (list_changed); a host-side
// accessor refreshes on its own thread, never on the reader.
struct Lists {
    std::vector<m::Tool>             tools;
    Clock::time_point                tools_fresh_until{};   // epoch: refresh on next read
    bool                             tools_stale = true;
    std::vector<m::Resource>         resources;
    std::vector<m::ResourceTemplate> templates;
    bool                             resources_stale = true;
    std::vector<m::Prompt>           prompts;
    bool                             prompts_stale = true;
};

}  // namespace agentty::mcp

MAYA_SENDABLE(agentty::mcp::Lists);
MAYA_SENDABLE(mcp::cap::CapabilityProvider::ListChangedFn);

namespace agentty::mcp {

struct Connection::Live {
    std::shared_ptr<void>      holder;   // what backs the channel; dropped after the peer
    std::unique_ptr<rpc::Peer> peer;
    maya::guarded<Lists>       lists;
    maya::guarded<bool>        poisoned;   // a cancelled call left it untrusted
    maya::guarded<bool>        busy;       // a call is in flight; calls go one at a time
    m::ClientHandlers          handlers;
    // Who to tell about a tool call's progress. One call at a time per
    // connection, so one slot is enough.
    maya::published<const std::function<void(std::string_view)>> progress;

    ~Live() {
        if (peer) peer->stop();
        peer.reset();
        holder.reset();
    }
};

namespace {
// One call at a time per connection, as the server sees it: progress is
// routed to the single in-flight call, and some servers can't interleave.
struct Turn {
    maya::guarded<bool>& busy;
    explicit Turn(maya::guarded<bool>& b) : busy(b) {
        busy.wait_with([](const bool& v) { return !v; }, [](bool& v) { v = true; });
    }
    ~Turn() { busy.with([](bool& v) { v = false; }); }
};

template <class R>
R take(std::expected<R, m::RpcError> r) {
    if (!r) throw r.error();
    return std::move(*r);
}
}  // namespace

Link stdio_link(::agentty::util::ChildProcess::Spawn spawn) {
    Link link;
    link.open = [spawn = std::move(spawn)]() -> Link::Opened {
        auto child = std::make_shared<::agentty::util::ChildProcess>(spawn);
        auto ch = rpc::stream_channel(child->out(), child->in());
        // On stop: EOF on its stdin, then kill it so its stdout closes and a
        // reader parked on it wakes. The child object lives on in `holder`
        // until after the peer has stopped.
        ch.interrupt = [c = child.get()] {
            c->close_stdin();
            c->terminate();
            c->interrupt_output();
        };
        return {std::move(ch), child};
    };
    link.alive = [](const std::shared_ptr<void>& holder) {
        return holder && static_cast<::agentty::util::ChildProcess*>(holder.get())->alive();
    };
    return link;
}

Connection::Connection(ConnectionConfig cfg, Link link)
    : cfg_(std::move(cfg)), link_(std::move(link)), origin_("mcp:" + cfg_.name) {
    live_.publish(connect());
}

Connection::~Connection() {
    // Stop the peer before anything its dispatch points at goes away.
    if (auto l = live_.current()) l->peer->stop();
    live_.publish(nullptr);
}

m::ClientHandlers Connection::handlers(std::weak_ptr<Live> weak) {
    m::ClientHandlers h;
    if (!cfg_.roots.empty())
        h.on_list_roots = [roots = cfg_.roots] { return m::ListRootsResult{roots, m::Json::object()}; };
    h.on_progress = [weak](const m::ProgressParams& u) {
        auto l = weak.lock();
        if (!l) return;
        auto sink = l->progress.current();
        if (!sink || !*sink) return;
        if (u.message && !u.message->empty()) { (*sink)(*u.message); return; }
        std::string text = "MCP progress: " + std::to_string(u.progress);
        if (u.total) text += " / " + std::to_string(*u.total);
        (*sink)(text);
    };
    h.on_resource_updated = cfg_.on_resource_updated;
    h.on_log              = cfg_.on_log;
    // list_changed only marks the cache stale (and tells the host); the next
    // host-side read refreshes. Refreshing here, on the reader, would wait
    // for a reply only the reader can deliver.
    auto changed = [this, weak](auto mark) {
        if (auto l = weak.lock()) l->lists.with(mark);
        if (auto fn = on_changed_.current(); fn && *fn) (*fn)();
    };
    h.on_tools_changed     = [changed] { changed([](Lists& s) { s.tools_stale = true; }); };
    h.on_resources_changed = [changed] { changed([](Lists& s) { s.resources_stale = true; }); };
    h.on_prompts_changed   = [changed] { changed([](Lists& s) { s.prompts_stale = true; }); };
    return h;
}

std::shared_ptr<Connection::Live> Connection::connect() {
    auto opened = link_.open();
    auto l = std::make_shared<Live>();
    l->holder   = std::move(opened.holder);
    l->handlers = handlers(l);

    rpc::Dispatch d;
    d.on_call = [weak = std::weak_ptr<Live>(l)](const m::Call& c) -> std::optional<std::string> {
        if (auto live = weak.lock()) return live->handlers.handle(c);
        return m::reply(c.id, std::unexpected(m::RpcError(m::errc::InternalError, "connection closed")));
    };
    d.on_notification = [weak = std::weak_ptr<Live>(l)](const m::Notification& n) {
        if (auto live = weak.lock()) live->handlers.handle(n);
    };
    l->peer = std::make_unique<rpc::Peer>(std::move(opened.channel), std::move(d), origin_);
    l->peer->set_default_timeout(cfg_.handshake_timeout);
    l->peer->start();

    m::ClientCapabilities client_caps;
    if (!cfg_.roots.empty()) client_caps.roots = m::RootsCapability{true};

    // 2026-07-28 servers read the protocol metadata from every request; older
    // ones ignore it. Then: server/discover if the server has it, else the
    // legacy initialize handshake.
    l->peer->set_request_meta(m::modern_request_meta(cfg_.client_info, client_caps));
    if (auto disc = l->peer->call<m::to_server::Discover>(m::DiscoverParams{})) {
        caps_ = disc->capabilities;
    } else {
        m::InitializeParams ip;
        ip.protocolVersion = std::string(m::kProtocolVersion);
        ip.capabilities    = client_caps;
        ip.clientInfo      = cfg_.client_info;
        auto init = take(l->peer->call<m::to_server::Initialize>(ip));
        caps_ = init.capabilities;
        l->peer->notify<m::to_server::Initialized>(m::Unit{});
    }

    refresh_tools(*l);
    if (caps_.resources) { try { refresh_resources(*l); } catch (...) {} }
    if (caps_.prompts)   { try { refresh_prompts(*l); } catch (...) {} }

    l->peer->set_default_timeout(cfg_.call_timeout);
    return l;
}

bool Connection::alive() const {
    auto l = live_.current();
    if (!l || l->peer->closed()) return false;
    return !link_.alive || link_.alive(l->holder);
}

std::expected<std::shared_ptr<Connection::Live>, std::string> Connection::ensure_live() {
    auto l = live_.current();
    const bool healthy = l && !l->peer->closed() && (!link_.alive || link_.alive(l->holder))
                         && !l->poisoned.read([](const bool& p) { return p; });
    if (healthy) return l;

    // One reconnect at a time; whoever loses the race uses the winner's.
    const bool mine = reconnecting_.with([](bool& busy) { return !std::exchange(busy, true); });
    if (!mine) {
        reconnecting_.wait_with([](const bool& busy) { return !busy; }, [](bool&) {});
        if (auto now = live_.current(); now && now != l) return now;
        return std::unexpected(std::string{"mcp server '" + origin_ + "' is not running"});
    }
    struct Done { Connection* c; ~Done() { c->reconnecting_.with([](bool& b) { b = false; }); } } done{this};
    try {
        if (auto old = live_.current()) old->peer->stop();
        live_.publish(nullptr);
        live_.publish(connect());
    } catch (const std::exception& e) {
        return std::unexpected(std::string{"mcp reconnect failed: "} + e.what());
    }
    if (auto fn = on_changed_.current(); fn && *fn) (*fn)();
    return live_.current();
}

void Connection::set_on_list_changed(ListChangedFn fn) {
    on_changed_.publish(std::make_shared<const ListChangedFn>(std::move(fn)));
}

// ── lists ───────────────────────────────────────────────────────────────────

void Connection::refresh_tools(Live& l) const {
    std::vector<m::Tool> all;
    m::Maybe<std::string> cursor;
    std::int64_t ttl = 0;
    do {
        m::ListToolsParams p;
        p.cursor = cursor;
        auto res = take(l.peer->call<m::to_server::ListTools>(p));
        for (auto& t : res.tools) all.push_back(std::move(t));
        cursor = res.nextCursor;
        if (res.ttlMs > 0 && ttl == 0) ttl = res.ttlMs;   // the first page's hint
    } while (cursor);
    const auto until = ttl > 0 ? Clock::now() + std::chrono::milliseconds(ttl) : Clock::time_point{};
    l.lists.with([](Lists& s, std::vector<m::Tool> t, Clock::time_point u) {
        s.tools = std::move(t);
        s.tools_fresh_until = u;
        s.tools_stale = false;
    }, std::move(all), until);
}

void Connection::refresh_resources(Live& l) const {
    std::vector<m::Resource> res;
    std::vector<m::ResourceTemplate> tpl;
    m::Maybe<std::string> cursor;
    do {
        m::ListResourcesParams p; p.cursor = cursor;
        auto r = take(l.peer->call<m::to_server::ListResources>(p));
        for (auto& x : r.resources) res.push_back(std::move(x));
        cursor = r.nextCursor;
    } while (cursor);
    try {
        cursor = m::Nothing;
        do {
            m::ListResourceTemplatesParams p; p.cursor = cursor;
            auto r = take(l.peer->call<m::to_server::ListResourceTemplates>(p));
            for (auto& x : r.resourceTemplates) tpl.push_back(std::move(x));
            cursor = r.nextCursor;
        } while (cursor);
    } catch (...) {}   // templates are optional
    l.lists.with([](Lists& s, std::vector<m::Resource> r, std::vector<m::ResourceTemplate> t) {
        s.resources = std::move(r);
        s.templates = std::move(t);
        s.resources_stale = false;
    }, std::move(res), std::move(tpl));
}

void Connection::refresh_prompts(Live& l) const {
    std::vector<m::Prompt> all;
    m::Maybe<std::string> cursor;
    do {
        m::ListPromptsParams p; p.cursor = cursor;
        auto r = take(l.peer->call<m::to_server::ListPrompts>(p));
        for (auto& x : r.prompts) all.push_back(std::move(x));
        cursor = r.nextCursor;
    } while (cursor);
    l.lists.with([](Lists& s, std::vector<m::Prompt> p) {
        s.prompts = std::move(p);
        s.prompts_stale = false;
    }, std::move(all));
}

std::vector<m::Tool> Connection::list() const {
    auto l = live_.current();
    if (!l) return {};
    const bool stale = l->lists.read([](const Lists& s, Clock::time_point now) {
        const bool ttl_lapsed = s.tools_fresh_until != Clock::time_point{} && now >= s.tools_fresh_until;
        return s.tools_stale || ttl_lapsed;
    }, Clock::now());
    if (stale) { try { refresh_tools(*l); } catch (...) {} }
    return l->lists.read([](const Lists& s) { return s.tools; });
}

std::vector<m::Resource> Connection::resources() const {
    auto l = live_.current();
    if (!l || !caps_.resources) return {};
    if (l->lists.read([](const Lists& s) { return s.resources_stale; })) {
        try { refresh_resources(*l); } catch (...) {}
    }
    return l->lists.read([](const Lists& s) { return s.resources; });
}

std::vector<m::ResourceTemplate> Connection::resource_templates() const {
    auto l = live_.current();
    if (!l || !caps_.resources) return {};
    if (l->lists.read([](const Lists& s) { return s.resources_stale; })) {
        try { refresh_resources(*l); } catch (...) {}
    }
    return l->lists.read([](const Lists& s) { return s.templates; });
}

std::vector<m::Prompt> Connection::prompts() const {
    auto l = live_.current();
    if (!l || !caps_.prompts) return {};
    if (l->lists.read([](const Lists& s) { return s.prompts_stale; })) {
        try { refresh_prompts(*l); } catch (...) {}
    }
    return l->lists.read([](const Lists& s) { return s.prompts; });
}

// ── calls ───────────────────────────────────────────────────────────────────

m::cap::Result Connection::execute(const m::cap::Request& req) {
    auto got = ensure_live();
    if (!got) return m::cap::Result::error(std::move(got.error()));
    auto l = std::move(*got);
    Turn turn(l->busy);

    l->progress.publish(std::make_shared<const std::function<void(std::string_view)>>(req.progress));
    struct Clear { Live& l; ~Clear() { l.progress.publish(nullptr); } } clear{*l};

    // A cancel mid-call closes the connection (the server can't be trusted
    // to be in a clean state) and fails the call; the next call reconnects.
    rpc::CallOptions o;
    o.cancel    = req.stop;
    o.on_cancel = [l] { l->poisoned.with([](bool& p) { p = true; }); };

    try {
        // MRTR: a 2026-07-28 server may answer input_required, asking us to
        // sample / elicit / list roots. Fulfil it with our handlers and retry.
        const auto mrtr = l->handlers.mrtr();
        m::Json params = m::to_json(m::CallToolParams{req.tool, req.args, m::Nothing, m::Json::object()});
        for (int round = 0; round < m::kMrtrMaxRounds; ++round) {
            auto c = l->peer->call_raw(m::to_server::CallTool::name, params, o);
            if (!c.outcome) {
                if (req.stop.stop_requested()) return m::cap::Result::error("cancelled");
                return m::cap::Result::error(std::string{"mcp call failed: "} + c.outcome.error().what());
            }
            auto retry = m::mrtr_retry(params, *c.outcome, mrtr);
            if (!retry) return m::cap::result_from_call(m::from_json<m::CallToolResult>(*c.outcome));
            params = std::move(*retry);
        }
        return m::cap::Result::error("mcp call failed: MRTR exceeded max rounds without a final result");
    } catch (const std::exception& e) {
        return m::cap::Result::error(std::string{"mcp call failed: "} + e.what());
    }
}

bool Connection::read_resource(const std::string& uri, std::vector<m::ResourceContents>& out,
                               std::string& err) {
    auto got = ensure_live();
    if (!got) { err = std::move(got.error()); return false; }
    Turn turn((*got)->busy);
    auto r = (*got)->peer->call<m::to_server::ReadResource>(m::ReadResourceParams{uri, m::Json::object()});
    if (!r) { err = std::string{"resources/read failed: "} + r.error().what(); return false; }
    out = std::move(r->contents);
    return true;
}

bool Connection::get_prompt(const std::string& name,
                            const std::vector<std::pair<std::string, std::string>>& args,
                            m::GetPromptResult& out, std::string& err) {
    auto got = ensure_live();
    if (!got) { err = std::move(got.error()); return false; }
    Turn turn((*got)->busy);
    m::GetPromptParams p;
    p.name = name;
    if (!args.empty()) p.arguments = args;
    auto r = (*got)->peer->call<m::to_server::GetPrompt>(p);
    if (!r) { err = std::string{"prompts/get failed: "} + r.error().what(); return false; }
    out = std::move(*r);
    return true;
}

}  // namespace agentty::mcp
