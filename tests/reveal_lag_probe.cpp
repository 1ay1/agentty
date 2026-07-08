// reveal_lag_probe — trace the md reveal cursor on a long turn to find the
// "first char of the block sticks, then bursts with the next tool use"
// stall. Uses only PUBLIC StreamingMarkdown accessors.
//
// It feeds a long markdown body that ends in a big fenced CODE BLOCK (a
// structured/eager block — the suspected stall shape), advancing the anim
// clock at 60fps, and prints reveal_in_progress + is_live each simulated
// 100ms so we can see whether the reveal glides or parks.

#include <cstdio>
#include <cstdlib>
#include <string>

#include <maya/core/anim_clock.hpp>
#include <maya/widget/markdown.hpp>

using maya::StreamingMarkdown;

int main() {
    const double feed_cps = std::getenv("FEED_CPS") ? std::atof(std::getenv("FEED_CPS")) : 800.0;
    const int    fps      = 60;
    const int    frame_ms = 1000 / fps;

    // Build a body: prose, then a large code block that arrives in ~1 burst
    // (models emit code fast). This is the eager-render tail case.
    std::string prose = "Here is the refactor and the resulting file.\n\n";
    for (int p = 0; p < 4; ++p)
        prose += "Paragraph " + std::to_string(p) +
                 " explains the change in enough words to wrap a couple of "
                 "rows in a normal terminal width so the reveal has work.\n\n";
    std::string code = "```cpp\n";
    const int code_lines = std::getenv("CODE_LINES") ? std::atoi(std::getenv("CODE_LINES")) : 60;
    for (int l = 0; l < code_lines; ++l)
        code += "    const auto v" + std::to_string(l) + " = compute(" + std::to_string(l) + ");\n";
    code += "```\n";

    StreamingMarkdown md;
    md.set_reveal_fx(true);
    md.set_live(true);
    md.set_reveal_pacing(90.0, 0.15);   // agentty prod

    std::string src;
    double owed = 0.0;
    std::size_t total = prose.size() + code.size();
    const double cps_per_frame = feed_cps * (frame_ms / 1000.0);

    std::printf("feed=%.0f cps  body=%zu bytes (prose=%zu code=%zu)\n\n",
                feed_cps, total, prose.size(), code.size());
    std::printf("%6s %8s %10s %8s %8s\n", "t(ms)", "src", "in_prog", "live", "final");
    std::printf("-------------------------------------------------\n");

    for (int f = 0; f < 60 * 30; ++f) {   // up to 30 simulated seconds
        owed += cps_per_frame;
        while (owed >= 1.0 && src.size() < total) {
            std::size_t take = std::min((std::size_t)owed, total - src.size());
            const std::string& all = (src.size() < prose.size())
                ? prose : code;
            // Append from the concatenated logical body.
            std::string body = prose + code;
            src = body.substr(0, std::min(body.size(), src.size() + take));
            owed -= take;
            (void)all;
        }
        md.set_content(src);
        maya::testing::advance_anim_clock_ms(frame_ms);

        // Simulate agentty's pre-emptive end-of-text drain: once the wire
        // has gone quiet (all bytes fed) for >= 120 ms and the reveal is
        // still catching up, the host calls request_finalize before any
        // tool card exists. `quiet_ms` tracks time since the last byte.
        static int quiet_ms = 0;
        const bool wire_done = (src.size() >= total);
        quiet_ms = wire_done ? (quiet_ms + frame_ms) : 0;
        if (wire_done && quiet_ms >= 120 && md.reveal_in_progress())
            md.request_finalize(160);

        (void)md.build();

        if (f % 6 == 0) {   // every 100ms
            std::printf("%6d %8zu %10d %8d %8d\n",
                        f * frame_ms, src.size(),
                        md.reveal_in_progress() ? 1 : 0,
                        md.is_live() ? 1 : 0,
                        md.is_finalizing() ? 1 : 0);
        }
    }
    std::printf("-------------------------------------------------\n");
    std::printf("final: src=%zu in_prog=%d live=%d\n",
                src.size(), md.reveal_in_progress(), md.is_live());
    return 0;
}
