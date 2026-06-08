// anthropic_md_stream — capture a real Anthropic SSE markdown stream once,
// then replay the recorded deltas through maya's StreamingMarkdown reveal-fx
// path so we can validate the typewriter animation against authentic bytes
// (delta sizes, inter-delta timing) without needing network access in CI.
//
// Modes:
//   --capture <out.jsonl> [--prompt "..."] [--model claude-...]
//       Performs ONE live call to api.anthropic.com using the same auth
//       resolution as the main binary (~/.config/agentty/credentials.json
//       or ANTHROPIC_API_KEY). Appends each StreamTextDelta as a JSON
//       line: {"t_ms": <int>, "delta": "<text>"}. t_ms is the monotonic
//       offset from the first delta.
//
//   --replay <in.jsonl> [--realtime] [--width 100] [--no-fx]
//       Reads the recorded deltas and pushes them into a StreamingMarkdown
//       with reveal_fx ON, rendering each frame to stdout. --realtime
//       sleeps between deltas at the recorded cadence; default is "as
//       fast as possible" but still polls render at 16 ms ticks so the
//       reveal cursor walks visibly.
//
// Intentionally minimal: this is a developer tool / bench, not a ctest
// entry. EXCLUDE_FROM_ALL in CMakeLists; build with
//   cmake --build build --target anthropic_md_stream

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <print>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "agentty/auth/auth.hpp"
#include "agentty/domain/conversation.hpp"
#include "agentty/provider/provider.hpp"
#include "agentty/provider/anthropic/transport.hpp"
#include "agentty/runtime/msg.hpp"

#include <maya/maya.hpp>
#include <maya/app/inline.hpp>
#include <maya/app/quit.hpp>
#include <maya/widget/markdown.hpp>

using namespace agentty;
using namespace std::chrono;
using json = nlohmann::json;

namespace {

// ── shared helpers ────────────────────────────────────────────────────────

[[nodiscard]] std::string escape_json_text(std::string_view s) {
    // nlohmann handles UTF-8 + control bytes correctly; round-trip via
    // json::value() is a one-liner instead of hand-rolling \u escapes.
    return json(s).dump();
}

void die(std::string_view msg, int code = 2) {
    std::println(stderr, "anthropic_md_stream: {}", msg);
    std::exit(code);
}

// ── capture ───────────────────────────────────────────────────────────────

constexpr const char* kDefaultPrompt =
    "Write a richly-formatted markdown response (about 800-1200 words) "
    "that exercises every shape: an H1 title, two H2 subsections, a "
    "bulleted list, a numbered list, an inline code span, a fenced code "
    "block (~15 lines of C++), a blockquote, a table with 3 columns and "
    "5 rows, and a closing paragraph with **bold** and *italic*. Topic: "
    "'a brief tour of CommonMark for terminal renderers'. Plain prose, "
    "no preamble.";

int do_capture(const std::string& out_path,
               const std::string& prompt,
               const std::string& model) {
    // Resolve creds exactly like main.cpp does.
    auth::Credentials creds = auth::resolve(/*cli_api_key=*/"");
    auth::AuthHeader  hdr   = auth::make_auth_header(creds);
    if (auth::is_empty(hdr)) {
        die("no credentials — set ANTHROPIC_API_KEY or run `agentty login`", 3);
    }

    Thread t;
    Message m;
    m.role = Role::User;
    m.text = prompt;
    t.messages.push_back(std::move(m));

    provider::anthropic::Request req;
    req.model         = model.empty() ? std::string{"claude-sonnet-4-5"} : model;
    req.system_prompt = provider::anthropic::default_system_prompt();
    req.messages      = t.messages;
    req.max_tokens    = provider::kSafeMaxTokens;
    req.auth          = hdr;

    std::ofstream out(out_path, std::ios::binary | std::ios::trunc);
    if (!out) die("cannot open output: " + out_path, 4);

    std::atomic<bool> finished{false};
    std::atomic<bool> failed{false};
    std::string       err_msg;
    std::size_t       delta_count = 0;
    std::size_t       total_bytes = 0;

    auto t0 = steady_clock::time_point::min();
    auto sink = [&](Msg m) {
        auto* sm = std::get_if<msg::StreamMsg>(&m);
        if (!sm) return;
        if (auto* d = std::get_if<StreamTextDelta>(sm)) {
            auto now = steady_clock::now();
            if (t0 == steady_clock::time_point::min()) t0 = now;
            const auto t_ms =
                duration_cast<milliseconds>(now - t0).count();
            out << R"({"t_ms":)" << t_ms
                << R"(,"delta":)" << escape_json_text(d->text)
                << "}\n";
            out.flush();
            ++delta_count;
            total_bytes += d->text.size();
            std::print(stderr, ".");
            std::fflush(stderr);
        } else if (std::get_if<StreamFinished>(sm)) {
            finished = true;
        } else if (auto* e = std::get_if<StreamError>(sm)) {
            failed   = true;
            err_msg  = e->message;
            finished = true;
        }
    };

    std::println(stderr, "→ capturing from api.anthropic.com (model={}) ...", req.model);
    provider::anthropic::run_stream_sync(std::move(req), sink, /*cancel=*/{});
    std::println(stderr, "");

    if (failed) {
        std::println(stderr, "stream error: {}", err_msg);
        return 5;
    }
    std::println(stderr, "captured {} deltas, {} bytes total → {}",
                 delta_count, total_bytes, out_path);
    return 0;
}

// ── replay ────────────────────────────────────────────────────────────────

struct Delta { long long t_ms; std::string text; };

std::vector<Delta> load_fixture(const std::string& path) {
    std::ifstream in(path);
    if (!in) die("cannot open fixture: " + path, 4);
    std::vector<Delta> out;
    std::string        line;
    while (std::getline(in, line)) {
        if (line.empty()) continue;
        try {
            auto j = json::parse(line);
            out.push_back({j.at("t_ms").get<long long>(),
                           j.at("delta").get<std::string>()});
        } catch (const std::exception& e) {
            die(std::string{"bad fixture line: "} + e.what(), 4);
        }
    }
    if (out.empty()) die("fixture has no deltas", 4);
    return out;
}

int do_replay(const std::string& in_path,
              bool realtime,
              int width,
              bool fx_on,
              double floor_cps,
              double drain_secs,
              double feed_cps) {
    auto deltas = load_fixture(in_path);
    std::println(stderr,
        "→ replay {} deltas (realtime={}, width={}, fx={}, cps={}, drain={}s, feed_cps={})",
        deltas.size(), realtime, width, fx_on, floor_cps, drain_secs,
        feed_cps > 0.0 ? std::to_string(feed_cps) : std::string{"∞"});

    // Shared md is fed by a producer thread and read by maya::live's
    // render thread. StreamingMarkdown::append() + build() are not
    // documented as thread-safe; for the dev tool case (single producer,
    // single consumer, append always before build sees it) the small
    // race window is acceptable. A mutex around append/build would add
    // contention and isn't necessary for visual validation.
    auto md = std::make_shared<maya::StreamingMarkdown>();
    md->set_live(true);
    md->set_reveal_fx(fx_on);
    md->set_reveal_pacing(floor_cps, drain_secs);

    std::atomic<bool> done{false};
    auto wall_start = steady_clock::now();

    // Flatten all deltas into one byte stream when --feed-cps is on:
    // we drip codepoint-by-codepoint at the requested rate. This is
    // what makes the typewriter feel REAL — the markdown parser's
    // commit_range can never settle a paragraph faster than the user
    // sees it appear, so committed prefixes don't flash in pre-styled.
    // Without this throttle, a 150-char burst delta lands all bytes
    // at once, the parser commits the paragraph (e.g. on `\n\n`), and
    // the rendered prefix paints with full style — maya's reveal
    // cursor only animates the still-live TAIL after that, so the
    // user perceives "everything appears then animation happens".
    std::string all_bytes;
    if (feed_cps > 0.0) {
        std::size_t total = 0;
        for (const auto& d : deltas) total += d.text.size();
        all_bytes.reserve(total);
        for (const auto& d : deltas) all_bytes.append(d.text);
    }

    std::thread producer([&, md] {
        if (feed_cps > 0.0) {
            // Codepoint-paced drip. UTF-8 codepoint boundary = byte
            // whose top 2 bits are not 10. Walk the byte stream,
            // releasing one cp at a time at the requested rate.
            const auto per_cp = duration_cast<nanoseconds>(
                duration<double>(1.0 / feed_cps));
            auto next_wake = steady_clock::now();
            std::size_t i = 0;
            while (i < all_bytes.size()) {
                std::size_t j = i + 1;
                while (j < all_bytes.size() &&
                       (static_cast<unsigned char>(all_bytes[j]) & 0xC0) == 0x80)
                    ++j;
                md->append(std::string_view{all_bytes.data() + i, j - i});
                i = j;
                next_wake += per_cp;
                std::this_thread::sleep_until(next_wake);
            }
        } else {
            for (std::size_t i = 0; i < deltas.size(); ++i) {
                if (realtime && i > 0) {
                    auto target = wall_start + milliseconds(deltas[i].t_ms);
                    std::this_thread::sleep_until(target);
                }
                md->append(deltas[i].text);
            }
        }
        // Let the reveal cursor catch up + chrome settle before quit.
        std::this_thread::sleep_for(2s);
        md->finish();
        std::this_thread::sleep_for(500ms);
        done = true;
        maya::quit();
    });

    maya::live({.fps = 30, .max_width = width}, [&] {
        return md->build();
    });

    producer.join();
    std::println(stderr, "→ replay done ({} deltas)", deltas.size());
    return 0;
}

void usage() {
    std::println(stderr,
        "anthropic_md_stream — capture/replay real Anthropic SSE for maya md tests\n"
        "\n"
        "  capture <out.jsonl> [--prompt \"...\"] [--model claude-...]\n"
        "  replay  <in.jsonl>  [--realtime | --feed-cps N]\n"
        "                      [--width N] [--no-fx]\n"
        "                      [--cps N] [--drain SECS]\n"
        "\n"
        "  --realtime     feed each delta at the recorded inter-delta gap.\n"
        "                 Each delta still lands as ONE append() — the\n"
        "                 markdown parser commits whole blocks instantly,\n"
        "                 so settled paragraphs appear in one go.\n"
        "  --feed-cps N   drip the stream byte-by-byte at N codepoints/sec\n"
        "                 (e.g. 20 = 50 ms per char). This is the REAL\n"
        "                 typewriter — commit_range can't settle a paragraph\n"
        "                 faster than chars appear, so the user sees every\n"
        "                 byte show up. Try `--feed-cps 25` first.\n"
        "  --cps N        floor cps of maya's own reveal cursor (default 120).\n"
        "  --drain SECS   target seconds to clear a burst backlog (default 0.8).\n");
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 3) { usage(); return 1; }
    std::string mode = argv[1];
    std::string path = argv[2];

    if (mode == "capture") {
        std::string prompt = kDefaultPrompt;
        std::string model;
        for (int i = 3; i < argc; ++i) {
            std::string a = argv[i];
            if (a == "--prompt" && i + 1 < argc) prompt = argv[++i];
            else if (a == "--model" && i + 1 < argc) model = argv[++i];
            else { usage(); return 1; }
        }
        return do_capture(path, prompt, model);
    }
    if (mode == "replay") {
        bool   realtime   = false, fx_on = true;
        int    width     = 100;
        double floor_cps = 120.0;
        double drain_secs = 0.8;
        double feed_cps   = 0.0;   // 0 = unthrottled (use delta cadence)
        for (int i = 3; i < argc; ++i) {
            std::string a = argv[i];
            if      (a == "--realtime") realtime = true;
            else if (a == "--no-fx")    fx_on    = false;
            else if (a == "--width"   && i + 1 < argc) width = std::atoi(argv[++i]);
            else if (a == "--cps"     && i + 1 < argc) floor_cps  = std::atof(argv[++i]);
            else if (a == "--drain"   && i + 1 < argc) drain_secs = std::atof(argv[++i]);
            else if (a == "--feed-cps" && i + 1 < argc) feed_cps  = std::atof(argv[++i]);
            else { usage(); return 1; }
        }
        return do_replay(path, realtime, width, fx_on,
                         floor_cps, drain_secs, feed_cps);
    }
    usage();
    return 1;
}
