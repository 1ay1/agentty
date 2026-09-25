// wire_encode_bench <thread-file> [runs] — per-round request-build cost.
//
// The save path is now incremental, so the remaining per-round work that
// still scales with thread size is building the wire body: every message
// re-serialised, every tool result re-capped, on every round. This runs on
// the stream worker, so it is pure added TTFT rather than UI lag — but it
// is paid once per agent step, so it shows up on long threads.
//
// Prints the cost of the three stages a turn pays, so a regression in any
// one of them is attributable.

#include <agentty/io/persistence.hpp>
#include <agentty/io/thread_log.hpp>
#include <agentty/provider/anthropic/transport.hpp>
#include <agentty/provider/wire_supersede.hpp>
#include <agentty/runtime/app/cmd_factory.hpp>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace agentty;
using clk = std::chrono::steady_clock;

namespace {
double ms(clk::duration d) {
    return std::chrono::duration<double, std::milli>(d).count();
}
double pct(std::vector<double> v, double p) {
    if (v.empty()) return 0;
    std::sort(v.begin(), v.end());
    return v[static_cast<std::size_t>(p * static_cast<double>(v.size() - 1))];
}
} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: wire_encode_bench <thread-file> [runs]\n");
        return 2;
    }
    const fs::path src{argv[1]};
    const int runs = argc > 2 ? std::atoi(argv[2]) : 10;

    std::optional<Thread> loaded;
    if (src.extension() == ".jsonl") {
        if (auto log = ThreadLog::open_path(src)) loaded = log->load_thread();
    } else if (auto t = persistence::load_thread_file(src)) {
        loaded = std::move(*t);
    }
    if (!loaded || loaded->messages.empty()) {
        std::fprintf(stderr, "wire_encode_bench: can't load %s\n", src.string().c_str());
        return 1;
    }
    const Thread& t = *loaded;

    std::vector<double> wire_ms, sup_ms, enc_ms;
    std::size_t body_bytes = 0, wire_msgs = 0;
    for (int i = 0; i < runs; ++i) {
        const auto a = clk::now();
        std::vector<Message> msgs = app::cmd::wire_messages_for(t);
        const auto b = clk::now();
        const auto superseded = provider::wire::superseded_read_ids(msgs);
        const auto c = clk::now();
        std::string body = provider::anthropic::messages_json_string(
            Thread{ThreadId{""}, "", msgs, {}, {}}, /*include_thinking=*/false);
        const auto d = clk::now();

        wire_ms.push_back(ms(b - a));
        sup_ms.push_back(ms(c - b));
        enc_ms.push_back(ms(d - c));
        body_bytes = body.size();
        wire_msgs  = msgs.size();
        (void)superseded;
    }

    std::printf("%s: %zu messages -> %zu on the wire, body %zu KB\n",
                src.filename().string().c_str(), t.messages.size(), wire_msgs,
                body_bytes / 1024);
    std::printf("  wire view build   p50 %7.2f ms  p90 %7.2f ms\n",
                pct(wire_ms, .5), pct(wire_ms, .9));
    std::printf("  superseded scan   p50 %7.2f ms  p90 %7.2f ms\n",
                pct(sup_ms, .5), pct(sup_ms, .9));
    std::printf("  body encode       p50 %7.2f ms  p90 %7.2f ms\n",
                pct(enc_ms, .5), pct(enc_ms, .9));
    std::printf("  TOTAL per round   p50 %7.2f ms\n",
                pct(wire_ms, .5) + pct(sup_ms, .5) + pct(enc_ms, .5));
    return 0;
}
