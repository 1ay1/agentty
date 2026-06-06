// agentty::acp::rpc::Peer — implementation. See jsonrpc.hpp for the design.

#include "agentty/acp/jsonrpc.hpp"

#include <stdexcept>
#include <string>
#include <utility>

namespace agentty::acp::rpc {

Peer::Peer(std::istream& in, std::ostream& out) : in_(in), out_(out) {}

void Peer::write_line(const nlohmann::json& msg) {
    // Compact, single-line dump + '\n'. flush so the client (Zed) sees the
    // line immediately — without the flush, libstdc++ buffers stdout and the
    // turn appears to hang.
    std::string s = msg.dump();
    std::lock_guard<std::mutex> lk(write_mtx_);
    out_ << s << '\n';
    out_.flush();
}

void Peer::notify(const std::string& method, const nlohmann::json& params) {
    write_line({
        {"jsonrpc", "2.0"},
        {"method", method},
        {"params", params},
    });
}

nlohmann::json Peer::request(const std::string& method, const nlohmann::json& params) {
    std::int64_t id;
    Pending slot;
    {
        std::lock_guard<std::mutex> lk(pending_mtx_);
        id = next_id_++;
        pending_[id] = &slot;
    }

    write_line({
        {"jsonrpc", "2.0"},
        {"id", id},
        {"method", method},
        {"params", params},
    });

    std::unique_lock<std::mutex> lk(slot.m);
    slot.cv.wait(lk, [&] { return slot.done; });

    // Slot is removed from the map by the reader before signalling, so it's
    // safe to let `slot` die when we return.
    if (slot.is_error)
        throw std::runtime_error(slot.error_message);
    return std::move(slot.result);
}

void Peer::respond(const nlohmann::json& id, const Outcome& outcome) {
    nlohmann::json msg{{"jsonrpc", "2.0"}, {"id", id}};
    if (outcome.is_error) {
        nlohmann::json err{{"code", outcome.error_code},
                           {"message", outcome.error_message}};
        if (!outcome.error_data.is_null()) err["data"] = outcome.error_data;
        msg["error"] = std::move(err);
    } else {
        msg["result"] = outcome.result;
    }
    write_line(msg);
}

void Peer::dispatch_inbound(nlohmann::json msg) {
    // A response to one of OUR outbound requests carries an id we minted and
    // a "result" or "error" but no "method".
    const bool has_method = msg.contains("method") && msg["method"].is_string();
    const bool has_id     = msg.contains("id") && !msg["id"].is_null();

    if (!has_method && has_id) {
        // It's a response. Correlate by integer id (we only ever mint ints).
        std::int64_t id = msg["id"].is_number_integer() ? msg["id"].get<std::int64_t>() : -1;
        Pending* slot = nullptr;
        {
            std::lock_guard<std::mutex> lk(pending_mtx_);
            if (auto it = pending_.find(id); it != pending_.end()) {
                slot = it->second;
                pending_.erase(it);
            }
        }
        if (!slot) return;  // unknown id — stray response, drop
        {
            std::lock_guard<std::mutex> sl(slot->m);
            if (msg.contains("error")) {
                slot->is_error = true;
                slot->error_message = msg["error"].value("message", "rpc error");
            } else {
                slot->result = msg.value("result", nlohmann::json(nullptr));
            }
            slot->done = true;
        }
        slot->cv.notify_one();
        return;
    }

    if (!has_method) return;  // malformed — neither request nor response

    const std::string method = msg["method"].get<std::string>();
    const nlohmann::json params =
        msg.contains("params") ? msg["params"] : nlohmann::json::object();

    if (has_id) {
        // Inbound request — we owe a response (unless the handler defers).
        rpc::Outcome out;
        if (request_handler_) {
            try {
                out = request_handler_(method, params, msg["id"]);
            } catch (const std::exception& e) {
                out = Outcome::fail(code::kInternalError, e.what());
            }
        } else {
            out = Outcome::fail(code::kMethodNotFound, "no handler installed");
        }
        // The handler returns rpc::deferred() to say "I'll respond later from a
        // worker via Peer::respond(id, ...)". Recognise that exact sentinel and
        // suppress the synchronous reply.
        const bool is_deferred = out.is_error
            && out.error_code == code::kInternalError
            && out.error_message == "__deferred__";
        if (!is_deferred) respond(msg["id"], out);
    } else {
        // Inbound notification — no response.
        if (notify_handler_) {
            try { notify_handler_(method, params); } catch (...) { /* swallow */ }
        }
    }
}

void Peer::run() {
    std::string line;
    while (std::getline(in_, line)) {
        if (line.empty()) continue;
        nlohmann::json msg;
        try {
            msg = nlohmann::json::parse(line);
        } catch (const std::exception&) {
            // Unparseable line — we can't recover an id, so emit a parse
            // error with null id per JSON-RPC.
            write_line({{"jsonrpc", "2.0"},
                        {"id", nullptr},
                        {"error", {{"code", code::kParseError},
                                   {"message", "parse error"}}}});
            continue;
        }
        dispatch_inbound(std::move(msg));
    }

    // Stream closed: wake any threads parked on outbound requests so they
    // don't hang forever waiting for a response that will never come.
    std::lock_guard<std::mutex> lk(pending_mtx_);
    for (auto& [id, slot] : pending_) {
        std::lock_guard<std::mutex> sl(slot->m);
        slot->is_error = true;
        slot->error_message = "connection closed";
        slot->done = true;
        slot->cv.notify_one();
    }
    pending_.clear();
}

} // namespace agentty::acp::rpc
