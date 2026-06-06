#pragma once
// agentty::acp::rpc — a minimal JSON-RPC 2.0 peer over a byte stream.
//
// ACP (the Agent Client Protocol that Zed speaks) frames messages as
// newline-delimited JSON: every request, response, and notification is a
// single JSON object on one line, terminated by '\n'. There is NO
// Content-Length header (that's LSP, not ACP). This matches the framing
// used by Zed, gemini-cli, and @zed-industries/claude-code-acp.
//
// This peer is bidirectional and full-duplex:
//   • The reader thread (run loop) parses inbound lines and routes them to
//     a handler — inbound *requests/notifications* the other side sent us.
//   • Any thread may call request()/notify()/respond() to send outbound
//     traffic. Writes are mutex-serialised so interleaved sends from the
//     turn worker and the reader never corrupt a line.
//
// Outbound *requests we originate* (e.g. session/request_permission,
// fs/read_text_file) block the calling thread on a condition variable until
// the matching response line arrives and the reader wakes us. That makes the
// permission round-trip a plain synchronous call from inside the headless
// turn loop, even though the bytes travel full-duplex.
//
// The peer is transport-agnostic: it reads from one std::istream and writes
// to one std::ostream. main() wires those to std::cin / std::cout; tests can
// wire them to stringstreams or a socketpair.

#include <condition_variable>
#include <cstdint>
#include <functional>
#include <istream>
#include <mutex>
#include <ostream>
#include <string>
#include <unordered_map>

#include <nlohmann/json.hpp>

namespace agentty::acp::rpc {

// JSON-RPC error codes (the standard set plus ACP's auth-required).
namespace code {
inline constexpr int kParseError     = -32700;
inline constexpr int kInvalidRequest = -32600;
inline constexpr int kMethodNotFound = -32601;
inline constexpr int kInvalidParams  = -32602;
inline constexpr int kInternalError  = -32603;
// ACP reserves -32000 for "authentication required" (the agent has no
// usable credentials and the client must drive `authenticate` first).
inline constexpr int kAuthRequired   = -32000;
} // namespace code

// The result of dispatching an inbound request. Exactly one of `result` /
// `error` is meaningful: if `is_error` is false the peer serialises
// {"result": result}; otherwise {"error": {code, message, data?}}.
struct Outcome {
    bool           is_error = false;
    nlohmann::json result;          // valid when !is_error
    int            error_code = 0;  // valid when is_error
    std::string    error_message;   // valid when is_error
    nlohmann::json error_data;      // optional; null = omitted

    static Outcome ok(nlohmann::json r) {
        return Outcome{false, std::move(r), 0, {}, nullptr};
    }
    static Outcome fail(int c, std::string msg, nlohmann::json data = nullptr) {
        return Outcome{true, nullptr, c, std::move(msg), std::move(data)};
    }
};

// Sentinel an inbound-request handler returns to say "I'll reply later from
// a worker thread via Peer::respond(id, ...)". The peer recognises this exact
// Outcome and does NOT write a synchronous response. Compare by identity of
// the marker fields (see jsonrpc.cpp).
[[nodiscard]] inline Outcome deferred() {
    return Outcome{true, nullptr, code::kInternalError, "__deferred__", nullptr};
}

// Handler invoked for every inbound request (id present) or notification
// (id absent). For notifications the returned Outcome is ignored.
//
// The handler runs ON THE READER THREAD. A long-running method (session/
// prompt drives a whole turn) MUST NOT block the reader: it should capture
// `id`, dispatch the work to a worker, and return rpc::deferred(). The worker
// later calls Peer::respond(id, ...). The `id` is passed so the handler can
// stash it for that deferred reply.
using RequestHandler =
    std::function<Outcome(const std::string& method,
                          const nlohmann::json& params,
                          const nlohmann::json& id)>;
using NotificationHandler =
    std::function<void(const std::string& method, const nlohmann::json& params)>;

class Peer {
public:
    Peer(std::istream& in, std::ostream& out);

    void on_request(RequestHandler h)      { request_handler_ = std::move(h); }
    void on_notification(NotificationHandler h) { notify_handler_ = std::move(h); }

    // Block the reader thread parsing inbound lines until EOF on `in`.
    // Each parsed message is routed: requests/notifications to the handler,
    // responses to the matching pending outbound request. Returns when the
    // input stream closes (client disconnected).
    void run();

    // ── Outbound, fire-and-forget ────────────────────────────────────────
    void notify(const std::string& method, const nlohmann::json& params);

    // ── Outbound request → blocks until the response arrives ─────────────
    // Returns the `result` payload on success; throws std::runtime_error
    // carrying the JSON-RPC error message on failure or stream close.
    nlohmann::json request(const std::string& method, const nlohmann::json& params);

    // ── Reply to an inbound request whose handling was deferred ──────────
    // Used when a method (session/prompt) is dispatched to a worker thread:
    // the handler returns nothing synchronously and the worker calls this
    // with the captured id once the turn completes.
    void respond(const nlohmann::json& id, const Outcome& outcome);

private:
    void write_line(const nlohmann::json& msg);
    void dispatch_inbound(nlohmann::json msg);

    std::istream& in_;
    std::ostream& out_;
    std::mutex    write_mtx_;

    RequestHandler      request_handler_;
    NotificationHandler notify_handler_;

    // Outbound request correlation. id → slot the caller is parked on.
    struct Pending {
        std::mutex              m;
        std::condition_variable cv;
        bool                    done = false;
        bool                    is_error = false;
        nlohmann::json          result;
        std::string             error_message;
    };
    std::mutex                                        pending_mtx_;
    std::unordered_map<std::int64_t, Pending*>        pending_;
    std::int64_t                                      next_id_ = 1;
};

} // namespace agentty::acp::rpc
