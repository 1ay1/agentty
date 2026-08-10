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
    (void)sink;
    die("capture path disabled in this build; use `det` or `replay` on a fixture", 6);
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
              double feed_cps,
              bool trace) {
    auto deltas = load_fixture(in_path);
    std::println(stderr,
        "→ replay {} deltas (realtime={}, width={}, fx={}, cps={}, drain={}s, feed_cps={}, trace={})",
        deltas.size(), realtime, width, fx_on, floor_cps, drain_secs,
        feed_cps > 0.0 ? std::to_string(feed_cps) : std::string{"∞"}, trace);

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

    if (trace) {
        // Trace mode bypasses maya::live and runs a manual render loop
        // that logs each frame's source/reveal/committed state to
        // stderr. Lets us SEE whether bursts come from the producer,
        // the reveal cursor, the commit, or the renderer.
        auto start = steady_clock::now();
        std::thread tprod([&, md] {
            if (feed_cps > 0.0) {
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
                        auto target = start + milliseconds(deltas[i].t_ms);
                        std::this_thread::sleep_until(target);
                    }
                    md->append(deltas[i].text);
                }
            }
            std::this_thread::sleep_for(1500ms);
            md->finish();
            done = true;
        });

        // Frame loop. Render each frame to a plain string (no ANSI),
        // measure how many visible chars are present, and log the
        // per-frame delta. If the typewriter is smooth this should
        // grow by ~floor_cps * 0.033 chars per 33 ms frame; if it's
        // bursty we'll see long flat stretches followed by big jumps.
        std::size_t last_visible = 0;
        int frame = 0;
        while (!done) {
            const maya::Element& el = md->build();
            std::string rendered = maya::render_to_string(el, width);
            // Count visible (non-whitespace, non-newline) chars as a
            // proxy for "what the user can see". Whitespace+newlines
            // are added by the renderer for layout and would muddy
            // the per-frame delta signal.
            std::size_t visible = 0;
            for (char c : rendered) {
                if (c != ' ' && c != '\n' && c != '\r' && c != '\t')
                    ++visible;
            }
            // Rendered height in rows = newline count + 1. This is the
            // signal that drives chrome position; oscillation here IS
            // the flicker.
            std::size_t rows = 1;
            for (char c : rendered) if (c == '\n') ++rows;
            static std::size_t last_rows = 0;
            const long long row_delta = static_cast<long long>(rows)
                                      - static_cast<long long>(last_rows);
            const auto t_ms = duration_cast<milliseconds>(
                steady_clock::now() - start).count();
            const long long delta = static_cast<long long>(visible)
                                  - static_cast<long long>(last_visible);
            std::println(stderr,
                "[{:>6} ms] frame={:>4} rows={:>4} Δrows={:+3} visible={:>5} Δ={:+4}",
                t_ms, frame, rows, row_delta, visible, delta);
            last_visible = visible;
            last_rows = rows;
            ++frame;
            std::this_thread::sleep_for(33ms);
        }
        tprod.join();
        std::println(stderr, "→ trace done ({} deltas, {} frames)", deltas.size(), frame);
        return 0;
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

// ── deterministic replay (the trustworthy measurement) ─────────────────────
// Single-threaded, frozen anim clock. Walks a virtual timeline in fixed
// frame steps; appends each recorded delta when its t_ms has passed; renders
// every frame and measures the per-frame VISIBLE-character delta. Because
// everything runs on one thread at a frozen clock, source / clip / cursor /
// visible are all consistent — no producer-thread race, no wall-clock jitter.
// This is what tells us, on REAL recorded bytes, whether the reveal glides.
int do_det(const std::string& in_path, int width, double floor_cps,
           double drain_secs, bool fx_on, int snap_at_frame, int snap_glide_ms) {
    std::vector<Delta> deltas = load_fixture(in_path);
    if (deltas.empty()) { std::println(stderr, "no deltas"); return 2; }

    maya::testing::freeze_anim_clock(0);
    maya::StreamingMarkdown md;
    md.set_live(true);
    md.set_reveal_fx(fx_on);
    md.set_reveal_pacing(floor_cps, drain_secs);

    auto visible_of = [&](const maya::Element& el) -> std::size_t {
        std::string s = maya::render_to_string(el, width);
        // Count real CONTENT codepoints only: skip newlines, spaces, and
        // (crucially) box-drawing / block-element chrome (U+2500..U+259F —
        // heading underline rules, table borders). Those are decoration the
        // renderer adds whole when a block exists; counting them as "visible"
        // would report a heading's full-width underline as a content burst.
        std::size_t v = 0;
        for (std::size_t i = 0; i < s.size(); ) {
            unsigned char c = static_cast<unsigned char>(s[i]);
            if ((c & 0x80) == 0) {            // ASCII
                if (c != '\n' && c != ' ') ++v;
                ++i;
            } else if ((c & 0xE0) == 0xC0) {  // 2-byte
                ++v; i += 2;
            } else if ((c & 0xF0) == 0xE0) {  // 3-byte
                // Decode to test the box-drawing/block range.
                char32_t cp = (c & 0x0F) << 12;
                if (i + 1 < s.size()) cp |= (static_cast<unsigned char>(s[i+1]) & 0x3F) << 6;
                if (i + 2 < s.size()) cp |= (static_cast<unsigned char>(s[i+2]) & 0x3F);
                if (!(cp >= 0x2500 && cp <= 0x259F)) ++v;   // skip chrome
                i += 3;
            } else {                          // 4-byte (emoji etc.)
                ++v; i += 4;
            }
        }
        return v;
    };

    constexpr std::int64_t kFrameMs = 16;
    const std::int64_t last_t = deltas.back().t_ms;
    const std::int64_t end_t  = last_t + 4000;   // drain tail

    std::size_t di = 0, prev_vis = 0;
    long long max_delta = 0, worst_t = 0;
    int frame = 0, over24 = 0;
    std::println(stderr,
        "→ det replay {} deltas (w={}, cps={}, drain={}s, fx={})",
        deltas.size(), width, floor_cps, drain_secs, fx_on);

    for (std::int64_t t = 0; t <= end_t; t += kFrameMs) {
        while (di < deltas.size() && deltas[di].t_ms <= t) {
            md.append(deltas[di].text);
            ++di;
        }
        if (di >= deltas.size()) md.request_finalize(200);
        // Simulate a TOOL BOUNDARY: agentty calls snap_reveal_to_edge() when
        // a tool card appears mid-stream (turn.cpp) — it jumps the reveal
        // cursor straight to the edge, pasting the whole typed-but-unrevealed
        // backlog in one frame. This is the burst the user sees on real
        // turns ("first char sticks, then it all appears with the next
        // tool") that a pure-text replay never triggers.
        if (snap_at_frame >= 0 && frame == snap_at_frame) {
            md.snap_reveal_to_edge(snap_glide_ms);
            std::println(stderr, "  >>> snap_reveal_to_edge({}) at frame {} "
                                 "(simulated tool boundary)", snap_glide_ms, frame);
        }
        const std::size_t vis = visible_of(md.build());
        const long long d = static_cast<long long>(vis)
                          - static_cast<long long>(prev_vis);
        if (d > max_delta) { max_delta = d; worst_t = t; }
        if (d > 24) ++over24;
        if (d != 0 || (frame % 30 == 0))
            std::println(stderr,
                "[{:>6} ms] f={:>4} vis={:>5} d={:+5} src={} clip={} cur={:.0f} cmt={} live={}",
                t, frame, vis, d,
                md.debug_source_size(), md.debug_reveal_byte_clip(),
                md.debug_reveal_cp(), md.debug_committed(), (int)md.is_live());
        prev_vis = vis;
        ++frame;
        maya::testing::advance_anim_clock_ms(kFrameMs);
        if (di >= deltas.size() && !md.is_live() && !md.is_finalizing()) break;
    }
    maya::testing::unfreeze_anim_clock();
    std::println(stderr,
        "→ det done: frames={} max_frame_delta={} (@ {} ms) frames_over_24={}",
        frame, max_delta, worst_t, over24);
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
    if (mode == "det") {
        int    width      = 100;
        double floor_cps  = 90.0;   // production default
        double drain_secs = 0.15;   // production default
        bool   fx_on      = true;
        int    snap_at    = -1;     // frame to fire a simulated tool-boundary snap
        int    snap_glide = 0;      // 0 = instant snap; >0 = bounded glide ms
        for (int i = 3; i < argc; ++i) {
            std::string a = argv[i];
            if      (a == "--no-fx")  fx_on = false;
            else if (a == "--width" && i + 1 < argc) width = std::atoi(argv[++i]);
            else if (a == "--cps"   && i + 1 < argc) floor_cps  = std::atof(argv[++i]);
            else if (a == "--drain" && i + 1 < argc) drain_secs = std::atof(argv[++i]);
            else if (a == "--snap-at" && i + 1 < argc) snap_at = std::atoi(argv[++i]);
            else if (a == "--snap-glide" && i + 1 < argc) snap_glide = std::atoi(argv[++i]);
            else { usage(); return 1; }
        }
        return do_det(path, width, floor_cps, drain_secs, fx_on, snap_at, snap_glide);
    }
    if (mode == "replay") {
        bool   realtime   = false, fx_on = true;
        int    width     = 100;
        double floor_cps = 120.0;
        double drain_secs = 0.8;
        double feed_cps   = 0.0;   // 0 = unthrottled (use delta cadence)
        bool   trace      = false;
        for (int i = 3; i < argc; ++i) {
            std::string a = argv[i];
            if      (a == "--realtime") realtime = true;
            else if (a == "--no-fx")    fx_on    = false;
            else if (a == "--trace")    trace    = true;
            else if (a == "--width"   && i + 1 < argc) width = std::atoi(argv[++i]);
            else if (a == "--cps"     && i + 1 < argc) floor_cps  = std::atof(argv[++i]);
            else if (a == "--drain"   && i + 1 < argc) drain_secs = std::atof(argv[++i]);
            else if (a == "--feed-cps" && i + 1 < argc) feed_cps  = std::atof(argv[++i]);
            else { usage(); return 1; }
        }
        return do_replay(path, realtime, width, fx_on,
                         floor_cps, drain_secs, feed_cps, trace);
    }
    usage();
    return 1;
}
