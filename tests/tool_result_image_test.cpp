// tool_result_image_test — a tool that surfaces an image (read on a PNG) must
// serialize its tool_result `content` as an ARRAY of blocks: the text note
// followed by an `image` block with the base64 data. This pins the vision path
// end of the pipeline (ToolUse::Done.images -> Anthropic wire).

#include "agtest.hpp"

#include "agentty/domain/conversation.hpp"
#include "agentty/provider/anthropic/transport.hpp"

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

// An assistant message with a single tool_use whose Done result carries `img`.
Thread thread_with_tool_image(const ImageContent& img, const std::string& text) {
    Message user; user.role = Role::User; user.text = "look at pic.png";

    Message asst; asst.role = Role::Assistant; asst.text = "";
    ToolUse tc;
    tc.id   = ToolCallId{"toolu_img"};
    tc.name = ToolName{"read"};
    tc.args = nlohmann::json{{"path", "pic.png"}};
    tc.status = ToolUse::Done{
        std::chrono::steady_clock::now(),
        std::chrono::steady_clock::now(),
        text,
        {img},
    };
    asst.tool_calls.push_back(std::move(tc));

    std::vector<Message> msgs;
    msgs.push_back(std::move(user));
    msgs.push_back(std::move(asst));
    return Thread{ThreadId{"t"}, "", std::move(msgs), {}, {}};
}

} // namespace

TEST_CASE("tool_result image block") {
    namespace ap = agentty::provider::anthropic;

    // ── A read-surfaced image becomes an image block in the tool_result ──
    {
        ImageContent img;
        img.media_type = "image/png";
        img.bytes      = std::string("\x89PNG\x0D\x0A\x1A\x0A", 8) +
                         std::string(32, '\x00');
        std::string wire =
            ap::messages_json_string(thread_with_tool_image(img, "[image pic.png]"));

        auto j = nlohmann::json::parse(wire);
        // Find the tool_result block for our call across all messages.
        nlohmann::json tr;
        for (const auto& msg : j)
            for (const auto& b : msg.value("content", nlohmann::json::array()))
                if (b.value("type", "") == "tool_result"
                    && b.value("tool_use_id", "") == "toolu_img")
                    tr = b;
        check(!tr.is_null(), "tool_result for the image call is emitted");

        // content must be an ARRAY (not a plain string) with an image block.
        check(tr["content"].is_array(), "image tool_result content is an array");
        bool has_image = false, has_text = false;
        std::string data;
        for (const auto& blk : tr["content"]) {
            const std::string ty = blk.value("type", "");
            if (ty == "image") {
                has_image = true;
                data = blk["source"].value("data", "");
                check(blk["source"].value("media_type", "") == "image/png",
                      "image block carries the media_type");
                check(blk["source"].value("type", "") == "base64",
                      "image block is base64-sourced");
            }
            if (ty == "text") has_text = true;
        }
        check(has_text, "text note precedes the image");
        check(has_image, "image block is present in the tool_result");
        check(!data.empty(), "image block carries non-empty base64 data");
    }

    // ── A tool with NO images still serializes content as a plain string ──
    {
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
        std::string wire = ap::messages_json_string(
            Thread{ThreadId{"t2"}, "", std::move(msgs), {}, {}});

        auto j = nlohmann::json::parse(wire);
        nlohmann::json tr;
        for (const auto& msg : j)
            for (const auto& b : msg.value("content", nlohmann::json::array()))
                if (b.value("type", "") == "tool_result"
                    && b.value("tool_use_id", "") == "toolu_txt")
                    tr = b;
        check(!tr.is_null(), "text tool_result is emitted");
        check(tr["content"].is_string(),
              "text-only tool_result keeps the plain-string content shape");
    }
}
