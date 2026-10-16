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
#include "agentty/provider/selection.hpp"
#include "agentty/runtime/app/update/internal.hpp"

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

namespace {

Message measured_turn(std::uint32_t input, std::uint32_t cache_read) {
    Message a;
    a.role = Role::Assistant;
    a.text = "ok";
    Message::Telemetry t;
    t.input_tokens = input;
    t.cache_read   = cache_read;
    a.telemetry    = t;
    return a;
}

Model model_with_gauge(int used) {
    Model m;
    m.s.context_max = 65536;
    m.s.tokens_in   = used;
    return m;
}

} // namespace

TEST_CASE("gauge: opening a thread shows that thread's last prompt, not the old one") {
    install_stub_deps();
    Thread t;
    t.id = ThreadId{"other"};
    Message u; u.role = Role::User; u.text = "hi";
    t.messages.push_back(u);
    t.messages.push_back(measured_turn(1000, 11000));   // 12k prompt
    t.messages.push_back(u);
    Message tail; tail.role = Role::Assistant; tail.text = "unmeasured";
    t.messages.push_back(tail);                        // newest turn has no usage

    auto start = model_with_gauge(60000);
    start.s.thread_loading = true;
    start.s.loading_thread = t.id;   // as ThreadListSelect does
    auto [m, _] = app::update(std::move(start), Msg{ThreadLoaded{std::move(t)}});
    CHECK(m.s.tokens_in == 12000);
}

TEST_CASE("thread load: a result landing after ^N doesn't replace the new thread") {
    install_stub_deps();
    auto start = model_with_gauge(60000);
    start.s.thread_loading = true;
    start.s.loading_thread = ThreadId{"other"};
    auto [fresh, c1] = app::update(std::move(start), Msg{NewThread{}});
    const ThreadId fresh_id = fresh.d.current.id;
    CHECK(!fresh.s.thread_loading);

    Thread late; late.id = ThreadId{"other"};
    Message u; u.role = Role::User; u.text = "old";
    late.messages.push_back(u);
    auto [m, c2] = app::update(std::move(fresh), Msg{ThreadLoaded{std::move(late)}});
    CHECK(m.d.current.id == fresh_id);
    CHECK(m.d.current.messages.empty());
}

TEST_CASE("gauge: a new thread starts at zero") {
    install_stub_deps();
    auto [m, _] = app::update(model_with_gauge(60000), Msg{NewThread{}});
    CHECK(m.s.tokens_in == 0);
}

TEST_CASE("gauge: a reading from before the latest compaction is not reused") {
    Thread t;
    Message u; u.role = Role::User; u.text = "hi";
    t.messages.push_back(u);
    t.messages.push_back(measured_turn(50000, 0));
    t.messages.push_back(u);
    Thread::CompactionRecord rec;
    rec.up_to_index = 2;   // the measured turn is now summarised away
    t.compactions.push_back(rec);
    CHECK(last_reported_prompt(t) == 0);

    t.messages.push_back(measured_turn(3000, 1000));
    CHECK(last_reported_prompt(t) == 4000);
}

TEST_CASE("gauge: per-turn usage takes a frame's input and cache parts together") {
    install_stub_deps();
    // Anthropic-style: message_start says cache_read 30k, the final frame
    // says the whole prompt was read fresh. The turn must not keep the 30k.
    Model m;
    Message user; user.role = Role::User; user.text = "hi";
    m.d.current.messages.push_back(user);
    Message asst; asst.role = Role::Assistant;
    m.d.current.messages.push_back(asst);
    StreamUsage first;  first.input_tokens = 2000;  first.cache_read_input_tokens = 30000;
    StreamUsage last;   last.input_tokens  = 32000; last.output_tokens = 5;
    for (const auto& su : {first, last}) {
        auto [next, _] = app::update(std::move(m), Msg{su});
        m = std::move(next);
    }
    CHECK(m.s.tokens_in == 32000);
    const auto& tm = m.d.current.messages.back().telemetry;
    CHECK(tm.has_value());
    CHECK(tm->input_tokens == 32000);
    CHECK(tm->cache_read == 0);
}

TEST_CASE("gauge: a finished compaction drops the gauge to the compacted size") {
    install_stub_deps();
    Model m;
    m.s.context_max = 65536;
    // A long thread whose last turn reported a 60k prompt.
    const std::string big(40000, 'x');
    for (int i = 0; i < 6; ++i) {
        Message u; u.role = Role::User; u.text = big;
        m.d.current.messages.push_back(u);
        m.d.current.messages.push_back(measured_turn(60000, 0));
    }
    m.s.tokens_in = 60000;
    auto step = [&](Msg msg) {
        auto [next, _] = app::update(std::move(m), std::move(msg));
        m = std::move(next);
    };
    step(Msg{CompactContext{}});
    CHECK(m.s.compacting);
    step(Msg{StreamTextDelta{"<summary>short</summary>"}});
    step(Msg{StreamFinished{StopReason::EndTurn}});
    CHECK_FALSE(m.s.compacting);
    CHECK(!m.d.current.compactions.empty());
    CHECK(m.s.tokens_in > 0);
    CHECK(m.s.tokens_in < 60000);
}

TEST_CASE("window: the ollama row clamps to num_ctx only on a real Ollama (#73)") {
    install_stub_deps();
    namespace oai = provider::openai;
    Model m;
    // A port nobody else uses, so this test owns its cache entry.
    m.d.selection = provider::parse_selection("ollama");
    m.d.selection.openai_endpoint.port = 18999;
    m.d.model_id = ModelId{"qwen2.5-coder-7b"};
    ModelInfo mi;
    mi.id = m.d.model_id;
    mi.context_window = 131072;     // what the server reported
    m.d.available_models.push_back(mi);

    // Not asked yet: treated as Ollama, which serves at most its num_ctx.
    CHECK(app::detail::resolved_context_max(m, "ollama") < 131072);

    // Asked, and it is a real Ollama daemon: still clamped.
    oai::set_speaks_native_for_test(m.d.selection.openai_endpoint, true);
    CHECK(app::detail::resolved_context_max(m, "ollama") < 131072);

    // Asked, and it is llama.cpp on that port: the window it serves.
    oai::set_speaks_native_for_test(m.d.selection.openai_endpoint, false);
    CHECK(app::detail::resolved_context_max(m, "ollama") == 131072);
}
