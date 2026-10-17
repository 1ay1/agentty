// src/mcp/connection.cpp — one MCP server as a capability provider. See
// connection.hpp.
//
// ── Lock order ─────────────────────────────────────────────────────────────
//
// Connection implements mcp-cpp's CapabilityProvider, whose virtuals
// (execute, list, read_resource, ...) have signatures fixed by a third-party
// library: they cannot be handed a capability. So this file takes NO jaal
// lock at all, and nothing in it can be part of a lock-order cycle:
//
//   lists        a jaal::snapshot (replaced wholesale on refresh)
//   poisoned     an atomic flag
//   busy         one call at a time: an atomic claimed by compare-and-swap.
//                A caller that loses blocks in atomic::wait until the
//                holder releases -- a wait on an atomic, not on a lock, so it
//                can't nest with any lock the caller holds into a cycle
//                (the holder never waits for anything the caller holds:
//                it waits only for the server's reply, delivered by the
//                peer's own reader thread).
//   reconnecting the same, for one reconnect at a time
//
// The blocking work -- an RPC round-trip -- runs on the caller's thread.
// The rpc::Peer needs `unlocked` for it, and that capability is minted at
// each library entry point below (`entry_root`), the one reviewed place.
// That is sound for the reason above: the peer's waits are for its own
// reader, which never takes any lock a caller of this class could hold.

#include "agentty/mcp/connection.hpp"
#include "agentty/mcp/client.hpp"

#include <atomic>
#include <thread>

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
MAYA_FROZEN(agentty::mcp::Lists);
MAYA_SENDABLE(mcp::cap::CapabilityProvider::ListChangedFn);

namespace agentty::mcp {

namespace {
// The capability root for a CapabilityProvider entry point (see the top of
// this file for why it is sound). Listed in tests/lint/allowlist.txt.
template <class F>
decltype(auto) entry_root(F&& f) {
    return maya::thread_root(std::forward<F>(f));
}

// Claim an atomic gate, blocking (atomic wait, holding nothing of ours)
// while someone else holds it. Every release goes through release() so the
// waiter is woken.
void claim(std::atomic<bool>& gate) {
    bool expected = false;
    while (!gate.compare_exchange_weak(expected, true, std::memory_order_acquire)) {
        if (expected) gate.wait(true, std::memory_order_acquire);
        expected = false;
    }
}
void release(std::atomic<bool>& gate) {
    gate.store(false, std::memory_order_release);
    gate.notify_all();
}
}  // namespace

struct Connection::Live {
    std::shared_ptr<void>      holder;   // what backs the channel; dropped after the peer
    std::unique_ptr<rpc::Peer> peer;
    ::mcp::ServerCapabilities  caps;     // set during connect(), read-only after
    maya::snapshot<Lists>      lists;
    std::atomic<bool>          poisoned{false};   // a cancelled call left it untrusted
    std::atomic<bool>          busy{false};       // a call is in flight; calls go one at a time
    m::ClientHandlers          handlers;
    // Who to tell about a tool call's progress. One call at a time per
    // connection, so one slot is enough.
    maya::published<const std::function<void(std::string_view)>> progress;
    // Who answers the server's elicitation/create for the in-flight call.
    maya::published<const std::function<m::Json(const m::Json&)>> elicit;

    ~Live() {
        // ~Peer stops and joins its tasks itself; the holder goes after it.
        peer.reset();
        holder.reset();
    }
};

namespace {
using ElicitDone = std::function<void(const std::string&)>;
// Set by an ACP editor that can open URL elicitations. Empty: form only.
maya::published<const ElicitDone>& url_elicit_done() {
    static maya::published<const ElicitDone> d;
    return d;
}
}  // namespace

void set_url_elicitation(std::function<void(const std::string&)> done) {
    url_elicit_done().publish(std::make_shared<const ElicitDone>(std::move(done)));
}

namespace {
// One call at a time per connection, as the server sees it: progress is
// routed to the single in-flight call, and some servers can't interleave.
struct Turn {
    std::atomic<bool>& busy;
    explicit Turn(std::atomic<bool>& b) : busy(b) { claim(busy); }
    ~Turn() { release(busy); }
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
        ch.interrupt = [c = child.get()](const maya::unlocked&) {
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
    live_.publish(entry_root([this](const maya::unlocked& cap) { return connect(cap); }));
}

Connection::~Connection() {
    // Stop the peer before anything its dispatch points at goes away:
    // dropping our handle runs ~Live -> ~Peer (stop + join) if we were the
    // last owner; a call still in flight holds its own handle until it ends.
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
    h.on_elicitation_complete = [](const m::ElicitationCompleteParams& p) {
        if (auto done = url_elicit_done().current(); done && *done) (*done)(p.elicitationId);
    };
    // A server asking the user something mid-call: pass it to whatever UI
    // the call came from (an editor's form). Nobody to ask: decline.
    h.on_elicit = [weak](const m::ElicitParams& p) -> m::ElicitResult {
        auto l = weak.lock();
        auto ask = l ? l->elicit.current() : nullptr;
        if (!ask || !*ask) return m::ElicitResult{m::ElicitAction::Decline, m::Nothing, m::Json::object()};
        return m::from_json<m::ElicitResult>((*ask)(m::to_json(p)));
    };
    // list_changed only marks the cache stale (and tells the host); the next
    // host-side read refreshes. Refreshing here, on the reader, would wait
    // for a reply only the reader can deliver.
    //
    // The host callback is reached through a shared copy of the slot, not
    // `this`: a call in flight keeps Live (and so the reader) running past
    // ~Connection, and a list_changed arriving then must not touch it.
    auto changed = [slot = on_changed_, weak](auto mark) {
        if (auto l = weak.lock()) (void)l->lists.update(mark);
        if (auto fn = slot->current(); fn && *fn) (*fn)();
    };
    h.on_tools_changed     = [changed] {
        changed([](const Lists& s) { Lists o = s; o.tools_stale = true; return o; });
    };
    h.on_resources_changed = [changed] {
        changed([](const Lists& s) { Lists o = s; o.resources_stale = true; return o; });
    };
    h.on_prompts_changed   = [changed] {
        changed([](const Lists& s) { Lists o = s; o.prompts_stale = true; return o; });
    };
    return h;
}

std::shared_ptr<Connection::Live> Connection::connect(const maya::unlocked& cap) {
    auto opened = link_.open();
    auto l = std::make_shared<Live>();
    l->holder   = std::move(opened.holder);
    l->handlers = handlers(l);

    rpc::Dispatch d;
    d.on_call = [weak = std::weak_ptr<Live>(l)](const maya::unlocked&,
                                                const m::Call& c) -> std::optional<std::string> {
        if (auto live = weak.lock()) return live->handlers.handle(c);
        return m::reply(c.id, std::unexpected(m::RpcError(m::errc::InternalError, "connection closed")));
    };
    d.on_notification = [weak = std::weak_ptr<Live>(l)](const maya::unlocked&,
                                                         const m::Notification& n) {
        if (auto live = weak.lock()) live->handlers.handle(n);
    };
    l->peer = std::make_unique<rpc::Peer>(std::move(opened.channel), std::move(d), origin_);
    l->peer->set_default_timeout(cfg_.handshake_timeout);
    l->peer->start();

    m::ClientCapabilities client_caps;
    if (!cfg_.roots.empty()) client_caps.roots = m::RootsCapability{true};
    // Form elicitation is relayed to the editor when one is attached; the
    // handler declines otherwise, which the spec allows. URL mode only when
    // the editor can open one.
    const auto url_done = url_elicit_done().current();
    const bool url_ok = url_done && *url_done;
    client_caps.elicitation = m::ElicitationCapability{
        m::Json::object(), url_ok ? m::Maybe<m::Json>{m::Json::object()} : m::Nothing};

    // 2026-07-28 servers read the protocol metadata from every request; older
    // ones ignore it. Then: server/discover if the server has it, else the
    // legacy initialize handshake.
    l->peer->set_request_meta(cap, m::modern_request_meta(cfg_.client_info, client_caps));
    if (auto disc = l->peer->call<m::to_server::Discover>(cap, m::DiscoverParams{})) {
        l->caps = disc->capabilities;
    } else {
        // A pre-2026 server. Speak its dialect all the way down: no modern
        // per-request _meta, the legacy version in initialize, and (over
        // HTTP) no MCP-Protocol-Version header until it answers -- a server
        // that rejects 2026-07-28 rejects it there too (issue #95).
        l->peer->set_request_meta(cap, m::Json::object());
        if (link_.set_protocol_version) link_.set_protocol_version(l->holder, {});
        m::InitializeParams ip;
        ip.protocolVersion = std::string(m::kLegacyProtocolVersion);
        ip.capabilities    = client_caps;
        ip.clientInfo      = cfg_.client_info;
        auto init = take(l->peer->call<m::to_server::Initialize>(cap, ip));
        l->caps = init.capabilities;
        // The version the server picked goes on every later request.
        if (link_.set_protocol_version)
            link_.set_protocol_version(l->holder, init.protocolVersion);
        l->peer->notify<m::to_server::Initialized>(cap, m::Unit{});
    }

    refresh_tools(cap, *l);
    if (l->caps.resources) { try { refresh_resources(cap, *l); } catch (...) {} }
    if (l->caps.prompts)   { try { refresh_prompts(cap, *l); } catch (...) {} }

    l->peer->set_default_timeout(cfg_.call_timeout);
    return l;
}

bool Connection::alive() const {
    auto l = live_.current();
    if (!l || entry_root([&](const maya::unlocked& c) { return l->peer->closed(c); })) return false;
    return !link_.alive || link_.alive(l->holder);
}

::mcp::ServerCapabilities Connection::server_capabilities() const {
    auto l = live_.current();
    return l ? l->caps : ::mcp::ServerCapabilities{};
}

std::expected<std::shared_ptr<Connection::Live>, std::string>
Connection::ensure_live(const maya::unlocked& cap) {
    const auto healthy = [&](const std::shared_ptr<Live>& x) {
        return x && !x->peer->closed(cap) && (!link_.alive || link_.alive(x->holder))
               && !x->poisoned.load(std::memory_order_acquire);
    };
    auto l = live_.current();
    if (healthy(l)) return l;

    // One reconnect at a time; whoever loses the race uses the winner's.
    bool expected = false;
    const bool mine = reconnecting_.compare_exchange_strong(expected, true, std::memory_order_acq_rel);
    if (!mine) {
        // Block until the winner finishes (atomic wait, not a sleep-poll),
        // then use whatever it left, if it is healthy.
        reconnecting_.wait(true, std::memory_order_acquire);
        if (auto now = live_.current(); healthy(now)) return now;
        return std::unexpected(std::string{"mcp server '" + origin_ + "' is not running"});
    }
    struct Done {
        Connection* c;
        ~Done() {
            c->reconnecting_.store(false, std::memory_order_release);
            c->reconnecting_.notify_all();
        }
    } done{this};
    // A previous winner may have finished between our health check and the
    // CAS: don't tear down its fresh connection.
    if (auto now = live_.current(); now != l && healthy(now)) return now;
    try {
        // Build the new connection first, then swap: a reader that races this
        // sees either the old Live (closed, it will come back here) or the new
        // one, never an empty slot it would report as "not running".
        auto old = live_.current();
        if (old) old->peer->stop(cap);
        live_.publish(connect(cap));
    } catch (const std::exception& e) {
        live_.publish(nullptr);
        return std::unexpected(std::string{"mcp reconnect failed: "} + e.what());
    }
    if (auto fn = on_changed_->current(); fn && *fn) (*fn)();
    return live_.current();
}

void Connection::set_on_list_changed(ListChangedFn fn) {
    on_changed_->publish(std::make_shared<const ListChangedFn>(std::move(fn)));
}

// ── lists ───────────────────────────────────────────────────────────────────

void Connection::refresh_tools(const maya::unlocked& cap, Live& l) const {
    std::vector<m::Tool> all;
    m::Maybe<std::string> cursor;
    std::int64_t ttl = 0;
    do {
        m::ListToolsParams p;
        p.cursor = cursor;
        auto res = take(l.peer->call<m::to_server::ListTools>(cap, p));
        for (auto& t : res.tools) all.push_back(std::move(t));
        cursor = res.nextCursor;
        if (res.ttlMs > 0 && ttl == 0) ttl = res.ttlMs;   // the first page's hint
    } while (cursor);
    const auto until = ttl > 0 ? Clock::now() + std::chrono::milliseconds(ttl) : Clock::time_point{};
    (void)l.lists.update([](const Lists& s, const std::vector<m::Tool>& t, const Clock::time_point& u) {
        Lists o = s;
        o.tools = t;
        o.tools_fresh_until = u;
        o.tools_stale = false;
        return o;
    }, std::move(all), until);
}

void Connection::refresh_resources(const maya::unlocked& cap, Live& l) const {
    std::vector<m::Resource> res;
    std::vector<m::ResourceTemplate> tpl;
    m::Maybe<std::string> cursor;
    do {
        m::ListResourcesParams p; p.cursor = cursor;
        auto r = take(l.peer->call<m::to_server::ListResources>(cap, p));
        for (auto& x : r.resources) res.push_back(std::move(x));
        cursor = r.nextCursor;
    } while (cursor);
    try {
        cursor = m::Nothing;
        do {
            m::ListResourceTemplatesParams p; p.cursor = cursor;
            auto r = take(l.peer->call<m::to_server::ListResourceTemplates>(cap, p));
            for (auto& x : r.resourceTemplates) tpl.push_back(std::move(x));
            cursor = r.nextCursor;
        } while (cursor);
    } catch (...) {}   // templates are optional
    (void)l.lists.update([](const Lists& s, const std::vector<m::Resource>& r,
                            const std::vector<m::ResourceTemplate>& t) {
        Lists o = s;
        o.resources = r;
        o.templates = t;
        o.resources_stale = false;
        return o;
    }, std::move(res), std::move(tpl));
}

void Connection::refresh_prompts(const maya::unlocked& cap, Live& l) const {
    std::vector<m::Prompt> all;
    m::Maybe<std::string> cursor;
    do {
        m::ListPromptsParams p; p.cursor = cursor;
        auto r = take(l.peer->call<m::to_server::ListPrompts>(cap, p));
        for (auto& x : r.prompts) all.push_back(std::move(x));
        cursor = r.nextCursor;
    } while (cursor);
    (void)l.lists.update([](const Lists& s, const std::vector<m::Prompt>& p) {
        Lists o = s;
        o.prompts = p;
        o.prompts_stale = false;
        return o;
    }, std::move(all));
}

std::vector<m::Tool> Connection::list() const {
    auto l = live_.current();
    if (!l) return {};
    const auto cur = l->lists.load();
    const auto now = Clock::now();
    const bool ttl_lapsed =
        cur->tools_fresh_until != Clock::time_point{} && now >= cur->tools_fresh_until;
    if (cur->tools_stale || ttl_lapsed) {
        try { entry_root([&](const maya::unlocked& c) { refresh_tools(c, *l); }); } catch (...) {}
    }
    return l->lists.load()->tools;
}

std::vector<m::Resource> Connection::resources() const {
    auto l = live_.current();
    if (!l || !l->caps.resources) return {};
    if (l->lists.load()->resources_stale) {
        try { entry_root([&](const maya::unlocked& c) { refresh_resources(c, *l); }); } catch (...) {}
    }
    return l->lists.load()->resources;
}

std::vector<m::ResourceTemplate> Connection::resource_templates() const {
    auto l = live_.current();
    if (!l || !l->caps.resources) return {};
    if (l->lists.load()->resources_stale) {
        try { entry_root([&](const maya::unlocked& c) { refresh_resources(c, *l); }); } catch (...) {}
    }
    return l->lists.load()->templates;
}

std::vector<m::Prompt> Connection::prompts() const {
    auto l = live_.current();
    if (!l || !l->caps.prompts) return {};
    if (l->lists.load()->prompts_stale) {
        try { entry_root([&](const maya::unlocked& c) { refresh_prompts(c, *l); }); } catch (...) {}
    }
    return l->lists.load()->prompts;
}

// ── calls ───────────────────────────────────────────────────────────────────

m::cap::Result Connection::execute(const m::cap::Request& req) {
    return entry_root([&](const maya::unlocked& cap) { return execute_impl(cap, req); });
}

m::cap::Result Connection::execute_impl(const maya::unlocked& cap, const m::cap::Request& req) {
    auto got = ensure_live(cap);
    if (!got) return m::cap::Result::error(std::move(got.error()));
    auto l = std::move(*got);
    Turn turn(l->busy);

    l->progress.publish(std::make_shared<const std::function<void(std::string_view)>>(req.progress));
    l->elicit.publish(req.elicit
        ? std::make_shared<const std::function<m::Json(const m::Json&)>>(req.elicit) : nullptr);
    struct Clear { Live& l; ~Clear() { l.progress.publish(nullptr); l.elicit.publish(nullptr); } } clear{*l};

    // A cancel mid-call closes the connection (the server can't be trusted
    // to be in a clean state) and fails the call; the next call reconnects.
    rpc::CallOptions o;
    o.cancel    = req.stop;
    o.on_cancel = [l](const maya::unlocked&) { l->poisoned.store(true, std::memory_order_release); };

    try {
        // MRTR: a 2026-07-28 server may answer input_required, asking us to
        // sample / elicit / list roots. Fulfil it with our handlers and retry.
        const auto mrtr = l->handlers.mrtr();
        m::Json params = m::to_json(m::CallToolParams{req.tool, req.args, m::Nothing, m::Json::object()});
        for (int round = 0; round < m::kMrtrMaxRounds; ++round) {
            auto c = l->peer->call_raw(cap, m::to_server::CallTool::name, params, o);
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
    return entry_root([&](const maya::unlocked& cap) {
    auto got = ensure_live(cap);
    if (!got) { err = std::move(got.error()); return false; }
    Turn turn((*got)->busy);
    auto r = (*got)->peer->call<m::to_server::ReadResource>(cap, m::ReadResourceParams{uri, m::Json::object()});
    if (!r) { err = std::string{"resources/read failed: "} + r.error().what(); return false; }
    out = std::move(r->contents);
    return true;
    });
}

bool Connection::get_prompt(const std::string& name,
                            const std::vector<std::pair<std::string, std::string>>& args,
                            m::GetPromptResult& out, std::string& err) {
    return entry_root([&](const maya::unlocked& cap) {
    auto got = ensure_live(cap);
    if (!got) { err = std::move(got.error()); return false; }
    Turn turn((*got)->busy);
    m::GetPromptParams p;
    p.name = name;
    if (!args.empty()) p.arguments = args;
    auto r = (*got)->peer->call<m::to_server::GetPrompt>(cap, p);
    if (!r) { err = std::string{"prompts/get failed: "} + r.error().what(); return false; }
    out = std::move(*r);
    return true;
    });
}

}  // namespace agentty::mcp
