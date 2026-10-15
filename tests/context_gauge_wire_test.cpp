// context_gauge_wire_test — the context gauge reads the real prompt size on
// every wire, cached or not (#96).
//
// OpenAI-style usage counts cached tokens INSIDE the prompt
// (prompt_tokens / input_tokens include cached_tokens). Anthropic counts them
// beside it. agentty used to read both the same way, so on any OpenAI-shaped
// server with a warm cache the gauge showed prompt + cached: a 33k context
// read as 65k (Strata, llama.cpp, vLLM, ChatGPT/Codex). Here the real SSE each
// server sends goes through the real transport and the real reducer, and the
// gauge has to land on the real prompt size.

#include <string>
#include <vector>

#include "agtest.hpp"

#include "agentty/domain/conversation.hpp"
#include "agentty/runtime/model.hpp"
#include "agentty/runtime/app/deps.hpp"
#include "agentty/runtime/app/update.hpp"
#include "agentty/store/store.hpp"
#include "agentty/provider/openai/transport.hpp"
#include "agentty/provider/responses/responses.hpp"
#include "agentty/provider/anthropic/transport.hpp"

using namespace agentty;

namespace {

store::Settings g_settings;

void install_stub_deps() {
    app::install_deps(app::Deps{
        .stream         = [](const maya::unlocked&, auto, auto) {},
        .save_thread    = [](const Thread&) {},
        .delete_thread  = [](const auto&) {},
        .load_threads   = [] { return std::vector<Thread>{}; },
        .load_thread    = [](const ThreadId&) { return std::optional<Thread>{}; },
        .load_settings  = [] { return g_settings; },
        .save_settings  = [](const store::Settings& s) { g_settings = s; },
        .new_thread_id  = [] { return ThreadId{"t-gauge"}; },
        .title_from     = [](std::string_view t) { return std::string{t}; },
        .auth           = {},
    });
}

// Feed every message a transport produced into the reducer and return the
// gauge reading.
int gauge_after(const std::vector<Msg>& msgs) {
    Model m;
    Message user;
    user.role = Role::User;
    user.text = "hello";
    m.d.current.messages.push_back(std::move(user));
    Message asst;
    asst.role = Role::Assistant;
    m.d.current.messages.push_back(std::move(asst));
    m.s.context_max = 65536;
    for (const auto& msg : msgs) {
        auto [next, _] = app::update(std::move(m), msg);
        m = std::move(next);
    }
    return m.s.tokens_in;
}

} // namespace

TEST_CASE("gauge: OpenAI chat usage with a warm cache is the prompt, not prompt + cache") {
    install_stub_deps();
    // Strata's last chat frame (serve/server.py openai chunks): prompt 33000,
    // 32000 of it already in its conversation cache.
    const std::string sse =
        "data: {\"choices\":[{\"delta\":{\"content\":\"ok\"}}]}\n\n"
        "data: {\"choices\":[{\"delta\":{},\"finish_reason\":\"stop\"}],"
        "\"usage\":{\"prompt_tokens\":33000,\"completion_tokens\":2,\"total_tokens\":33002,"
        "\"prompt_tokens_details\":{\"cached_tokens\":32000}}}\n\n"
        "data: [DONE]\n\n";
    CHECK(gauge_after(provider::openai::parse_sse_for_test(sse)) == 33000);
}

TEST_CASE("gauge: Responses (ChatGPT/Codex) usage with a warm cache is the prompt") {
    install_stub_deps();
    // Strata's serve/responses.py response.completed usage.
    const std::vector<std::string> sse = {
        R"({"type":"response.output_text.delta","delta":"ok"})",
        R"({"type":"response.completed","response":{"status":"completed",)"
        R"("usage":{"input_tokens":33000,"input_tokens_details":{"cached_tokens":32000},)"
        R"("output_tokens":2,"total_tokens":33002}}})",
    };
    CHECK(gauge_after(provider::responses::parse_sse_for_test(sse)) == 33000);
}

TEST_CASE("gauge: Anthropic usage keeps adding the cache, which it reports beside input") {
    install_stub_deps();
    // Strata's Anthropic shape: message_start has the whole prompt, the final
    // message_delta splits it into input + cache_read.
    const std::vector<std::pair<std::string, std::string>> sse = {
        {"message_start", R"({"type":"message_start","message":{"id":"m","type":"message",)"
                          R"("role":"assistant","content":[],"usage":{"input_tokens":33000,"output_tokens":0}}})"},
        {"message_delta", R"({"type":"message_delta","delta":{"stop_reason":"end_turn"},)"
                          R"("usage":{"input_tokens":1000,"cache_read_input_tokens":32000,"output_tokens":2}})"},
        {"message_stop",  R"({"type":"message_stop"})"},
    };
    CHECK(gauge_after(provider::anthropic::parse_sse_for_test(sse)) == 33000);
}
