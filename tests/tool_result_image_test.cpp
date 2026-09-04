// tool_result_image_test — a tool that surfaces an image (read on a PNG) must
// serialize the picture into its tool_result on EVERY wire dialect, not just
// one. This pins the SSOT: the image POLICY (which images, empty-skip,
// media-type default) lives once in provider::wire, and each dialect renders
// it in its own JSON shape. The cross-dialect loop is the guard that the
// Anthropic-only drift (which shipped once) can't come back.

#include "agtest.hpp"

#include "agentty/domain/conversation.hpp"
#include "agentty/provider/anthropic/transport.hpp"
#include "agentty/provider/ollama/transport.hpp"
#include "agentty/provider/provider.hpp"
#include "agentty/provider/responses/responses.hpp"

#include <nlohmann/json.hpp>

using agentty::ImageContent;
using agentty::Message;
using agentty::Role;
using agentty::Thread;
using agentty::ThreadId;
using agentty::ToolCallId;
using agentty::ToolName;
using agentty::ToolUse;

namespace {

// A tiny valid-looking PNG (magic bytes + a little body).
ImageContent png() {
    ImageContent img;
    img.media_type = "image/png";
    img.bytes = std::string("\x89PNG\x0D\x0A\x1A\x0A", 8) + std::string(32, '\x00');
    return img;
}

// User turn + assistant tool_use whose Done result carries `img`.
std::vector<Message> tool_image_messages(const ImageContent& img,
                                         const std::string& note) {
    Message user; user.role = Role::User; user.text = "look at pic.png";
    Message asst; asst.role = Role::Assistant; asst.text = "";
    ToolUse tc;
    tc.id   = ToolCallId{"toolu_img"};
    tc.name = ToolName{"read"};
    tc.args = nlohmann::json{{"path", "pic.png"}};
    tc.status = ToolUse::Done{std::chrono::steady_clock::now(),
                              std::chrono::steady_clock::now(), note, {img}};
    asst.tool_calls.push_back(std::move(tc));
    std::vector<Message> msgs;
    msgs.push_back(std::move(user));
    msgs.push_back(std::move(asst));
    return msgs;
}

// Recursively true iff any object in `j` has "type" == one of the image
// markers each dialect uses (Anthropic "image", OpenAI "input_image") OR
// carries a non-empty ollama-style top-level "images" array.
bool json_has_image(const nlohmann::json& j) {
    if (j.is_object()) {
        const std::string ty = j.value("type", "");
        if (ty == "image" || ty == "input_image") return true;
        if (auto it = j.find("images");
            it != j.end() && it->is_array() && !it->empty())
            return true;
        for (auto& [k, v] : j.items()) if (json_has_image(v)) return true;
    } else if (j.is_array()) {
        for (const auto& e : j) if (json_has_image(e)) return true;
    }
    return false;
}

} // namespace

TEST_CASE("tool_result image on every wire dialect") {
    namespace ap  = agentty::provider::anthropic;
    namespace oll = agentty::provider::ollama;
    namespace rsp = agentty::provider::responses;

    const ImageContent img = png();

    // ── Anthropic Messages ──────────────────────────────────────────────
    {
        Thread t{ThreadId{"t"}, "", tool_image_messages(img, "[image pic.png]"), {}, {}};
        auto j = nlohmann::json::parse(ap::messages_json_string(t));
        // Find our tool_result and assert its content is an array w/ an image.
        nlohmann::json tr;
        for (const auto& msg : j)
            for (const auto& b : msg.value("content", nlohmann::json::array()))
                if (b.value("type", "") == "tool_result"
                    && b.value("tool_use_id", "") == "toolu_img")
                    tr = b;
        check(!tr.is_null(), "anthropic: tool_result emitted");
        check(tr["content"].is_array(), "anthropic: image tool_result is an array");
        check(json_has_image(tr["content"]), "anthropic: image block present");
    }

    // ── OpenAI Responses (ChatGPT/Codex + hosted OpenAI) ────────────────
    {
        agentty::provider::Request req;
        req.messages = tool_image_messages(img, "[image pic.png]");
        auto input = rsp::build_input(req);
        // The function_call_output for our call must carry the image.
        bool found = false;
        for (const auto& item : input) {
            if (item.value("type", "") == "function_call_output"
                && item.value("call_id", "") == "toolu_img") {
                found = true;
                check(item["output"].is_array(),
                      "responses: image tool output is a content array");
                check(json_has_image(item["output"]),
                      "responses: input_image part present");
            }
        }
        check(found, "responses: function_call_output for the call emitted");
    }

    // ── ollama / OpenAI-chat (native role:\"tool\") ─────────────────────
    {
        auto arr = oll::build_messages(tool_image_messages(img, "[image pic.png]"),
                                       /*json_protocol=*/false);
        bool found = false;
        for (const auto& m : arr) {
            if (m.value("role", "") == "tool") {
                found = true;
                check(m.contains("images") && m["images"].is_array()
                          && !m["images"].empty(),
                      "ollama: tool result carries an images array");
            }
        }
        check(found, "ollama: role:tool result emitted");
    }
}

TEST_CASE("text-only tool_result keeps the plain shape") {
    namespace ap = agentty::provider::anthropic;

    Message user; user.role = Role::User; user.text = "run it";
    Message asst; asst.role = Role::Assistant; asst.text = "";
    ToolUse tc;
    tc.id = ToolCallId{"toolu_txt"};
    tc.name = ToolName{"shell"};
    tc.args = nlohmann::json{{"command", "echo hi"}};
    tc.status = ToolUse::Done{std::chrono::steady_clock::now(),
                              std::chrono::steady_clock::now(), "hi", {}};
    asst.tool_calls.push_back(std::move(tc));
    std::vector<Message> msgs;
    msgs.push_back(std::move(user));
    msgs.push_back(std::move(asst));

    auto j = nlohmann::json::parse(
        ap::messages_json_string(Thread{ThreadId{"t2"}, "", std::move(msgs), {}, {}}));
    nlohmann::json tr;
    for (const auto& msg : j)
        for (const auto& b : msg.value("content", nlohmann::json::array()))
            if (b.value("type", "") == "tool_result"
                && b.value("tool_use_id", "") == "toolu_txt")
                tr = b;
    check(!tr.is_null(), "text tool_result emitted");
    check(tr["content"].is_string(),
          "text-only tool_result keeps plain-string content");
}
