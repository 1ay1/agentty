// transcript_bound_test — write_thread_transcript_md is BOUNDED, recency-
// biased, and correct (src/io/persistence.cpp).
//
// The transcript is a fork's on-disk memory of its parent, read by the model
// on demand. It must stay a small, useful, greppable artifact even when the
// parent thread is enormous. This test pins:
//   1. A huge thread produces a size-CAPPED transcript (not multi-MB).
//   2. Recency bias: the NEWEST turns survive; the OLDEST are elided with a
//      visible marker (a fork most likely needs recent context).
//   3. A single giant `text` block is clipped head+tail (can't dominate).
//   4. Tool calls collapse to one `› tool(name)` line; tool OUTPUT is never
//      written (the heaviest bytes of a real thread).
//   5. smart_routing view-only cards are skipped.
//   6. Output is valid UTF-8 even from arbitrary input bytes.
//   7. A tiny thread round-trips verbatim (no elision when it fits).

#include "agtest.hpp"

#include "agentty/io/persistence.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include <unistd.h>

namespace fs = std::filesystem;
using namespace agentty;


static std::string slurp(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

static Message user_msg(std::string text) {
    Message m;
    m.role = Role::User;
    m.text = std::move(text);
    return m;
}

static Message asst_msg(std::string text) {
    Message m;
    m.role = Role::Assistant;
    m.text = std::move(text);
    return m;
}

TEST_CASE("transcript bound") {
    auto tmp = fs::temp_directory_path()
             / ("agentty_transcript_test_" + std::to_string(::getpid()));
    fs::remove_all(tmp);
    fs::create_directories(tmp);
    ::setenv("HOME", tmp.c_str(), 1);
    ::unsetenv("USERPROFILE");

    // ── 1+2+3: a huge thread is capped, recency-biased, per-msg clipped ──
    {
        Thread t;
        t.id = persistence::new_id();
        t.title = "huge";
        // 4000 messages, ~2 KB each ≈ 8 MB of raw text — far over the 512 KB
        // cap. Each message is uniquely tagged so we can prove WHICH survived.
        for (int i = 0; i < 4000; ++i) {
            std::string body = "TAG" + std::to_string(i) + " ";
            body += std::string(2000, 'x');
            t.messages.push_back(i % 2 ? asst_msg(body) : user_msg(body));
        }
        // A single oversized paste in the NEWEST message (must be clipped,
        // not dropped, and not blow the budget).
        Message big = user_msg("HEAD_MARKER " + std::string(400 * 1024, 'Z')
                               + " TAIL_MARKER");
        t.messages.push_back(std::move(big));

        auto path = persistence::write_thread_transcript_md(t);
        check(!path.empty(), "huge thread: transcript written");
        std::string md = slurp(path);

        // 1. Total size capped (allow header + markers slack).
        check(md.size() <= 512 * 1024 + 4096,
              "huge thread: total size capped (~512 KB), got "
              + std::to_string(md.size()) + " bytes");

        // 2. Recency: newest tags present, oldest tags gone.
        check(md.find("TAG3999") != std::string::npos,
              "recency: newest content kept");
        check(md.find("TAG0 ") == std::string::npos,
              "recency: oldest content elided");
        check(md.find("older turns elided") != std::string::npos,
              "recency: elision marker present");
        check(md.find("4001 messages total") != std::string::npos,
              "header reports full message count");

        // 3. The giant paste was clipped head+tail, not dropped whole.
        check(md.find("HEAD_MARKER") != std::string::npos,
              "clip: oversized text keeps its head");
        check(md.find("TAIL_MARKER") != std::string::npos,
              "clip: oversized text keeps its tail");
        check(md.find("bytes elided") != std::string::npos,
              "clip: per-message elision marker present");
    }

    // ── 4: tool calls collapse; tool OUTPUT is never written ──
    {
        Thread t;
        t.id = persistence::new_id();
        t.title = "tools";
        Message a = asst_msg("running a command");
        ToolUse tc;
        tc.name = ToolName{"shell"};
        tc.args = nlohmann::json{{"command", "ls"}};
        // Simulate a huge captured output — must NOT appear in the transcript.
        tc.status = ToolUse::Done{ {}, {}, std::string(100 * 1024, 'O') };
        a.tool_calls.push_back(std::move(tc));
        t.messages.push_back(std::move(a));

        auto path = persistence::write_thread_transcript_md(t);
        std::string md = slurp(path);
        check(md.find("\xe2\x80\xba tool(shell)") != std::string::npos,
              "tool call collapses to one line");
        check(md.find(std::string(200, 'O')) == std::string::npos,
              "tool OUTPUT is never written to the transcript");
    }

    // ── 5: smart_routing view-only cards are skipped ──
    {
        Thread t;
        t.id = persistence::new_id();
        t.title = "routing";
        Message card;
        card.role = Role::User;
        card.smart_routing = true;
        card.smart_route_note = "ROUTING_CARD_NOISE";
        t.messages.push_back(std::move(card));
        t.messages.push_back(user_msg("real question"));

        auto path = persistence::write_thread_transcript_md(t);
        std::string md = slurp(path);
        check(md.find("ROUTING_CARD_NOISE") == std::string::npos,
              "smart_routing card is skipped");
        check(md.find("real question") != std::string::npos,
              "real turn still present");
    }

    // ── 6: arbitrary input bytes → valid UTF-8 output ──
    {
        Thread t;
        t.id = persistence::new_id();
        t.title = "utf8";
        // A lone continuation byte + a truncated 2-byte lead: invalid UTF-8.
        t.messages.push_back(user_msg(std::string("bad\x80\xC3 end", 8)));
        auto path = persistence::write_thread_transcript_md(t);
        std::string md = slurp(path);
        // to_valid_utf8 substitutes U+FFFD for the bad bytes; assert the
        // OUTPUT is well-formed UTF-8 (no lone continuation / truncated lead
        // can reach the .md) rather than guessing the scrub's exact bytes.
        check(!md.empty(), "invalid-UTF-8 input still produces a transcript");
        auto is_valid_utf8 = [](const std::string& s) {
            std::size_t i = 0, n = s.size();
            while (i < n) {
                unsigned char c = s[i];
                std::size_t len = c < 0x80 ? 1 : (c >> 5) == 0x6 ? 2
                                : (c >> 4) == 0xE ? 3 : (c >> 3) == 0x1E ? 4 : 0;
                if (len == 0 || i + len > n) return false;
                for (std::size_t k = 1; k < len; ++k)
                    if ((static_cast<unsigned char>(s[i + k]) & 0xC0) != 0x80) return false;
                i += len;
            }
            return true;
        };
        check(is_valid_utf8(md),
              "transcript is well-formed UTF-8 even from garbage input");
    }

    // ── 7: a small thread fits verbatim, no elision ──
    {
        Thread t;
        t.id = persistence::new_id();
        t.title = "small";
        t.messages.push_back(user_msg("hello"));
        t.messages.push_back(asst_msg("hi there"));
        auto path = persistence::write_thread_transcript_md(t);
        std::string md = slurp(path);
        check(md.find("hello") != std::string::npos
           && md.find("hi there") != std::string::npos,
              "small thread: both turns present verbatim");
        check(md.find("older turns elided") == std::string::npos,
              "small thread: no elision marker");
        check(md.find("2 messages total") != std::string::npos,
              "small thread: count correct");
    }

    fs::remove_all(tmp);
}
