// stream_async_freeze_test.cpp — reproduces the "md streaming stops
// partway through a long turn, fine again next turn" freeze and pins the
// fix.
//
// ROOT CAUSE (as it existed before the fix): cached_markdown_for fed the
// LIVE combined source (msg.text + streaming_text + pending_stream) to
// the StreamingMarkdown widget via set_content_async. That routing sends
// any change that is (a) a divergent-prefix change AND (b) >= 16 KB to a
// detached background parse worker; while the worker runs, is_parsing()
// is true and build() returns the PREVIOUS element tree — the reveal is
// FROZEN until the foreground polls build() again AND the worker has
// landed (maybe_apply_async_ only runs from build()).
//
// The trigger in a long turn is the SUB-TURN BOUNDARY: the prior
// sub-turn's streaming_text folds into msg.text and streaming_text
// resets, so the next combined feed is a divergent-prefix change (the
// tail bytes differ). Once the accumulated body is > 16 KB that swap
// spawns the worker → stall. Next turn starts on a fresh short
// placeholder (< 16 KB) which takes the sync path → looks fine again.
//
// This test drives the widget DIRECTLY (no I/O, no model) at the exact
// seam cached_markdown_for hits:
//
//   1. async_divergent_over_16k_freezes_reveal
//        Confirms the BUG is real at the widget level: a >= 16 KB
//        divergent-prefix set_content_async DOES enter is_parsing() and
//        build() keeps returning the OLD content while the worker runs.
//        (This is the mechanism the fix removes from the live path.)
//
//   2. sync_set_content_never_freezes
//        Confirms the FIX: the same >= 16 KB divergent-prefix change fed
//        via set_content (what cached_markdown_for now does on the live
//        !settled path) NEVER enters is_parsing() and build() reflects
//        the new content IMMEDIATELY, same frame.
//
// Links maya only (header-driven StreamingMarkdown), same as
// reveal_pacing_test.

#include "maya/widget/markdown.hpp"

#include <chrono>
#include <cstdio>
#include <string>
#include <thread>

using maya::StreamingMarkdown;

namespace {

int g_failures = 0;
int g_checks   = 0;

#define CHECK(cond, msg)                                                    \
    do {                                                                    \
        ++g_checks;                                                         \
        if (!(cond)) {                                                      \
            ++g_failures;                                                   \
            std::fprintf(stderr, "  FAIL [%s:%d] %s\n",                     \
                         __FILE__, __LINE__, (msg));                        \
        }                                                                   \
    } while (0)

// Build a markdown body of roughly `kb` kilobytes whose bytes differ from
// `other` at the tail (so a prefix-preserving append detector treats it
// as a DIVERGENT-prefix change, exactly like a sub-turn boundary where
// streaming_text reset and the tail text changed). `tag` seeds the
// content so two calls with different tags diverge.
std::string big_body(int kb, char tag) {
    std::string s;
    s.reserve(static_cast<std::size_t>(kb) * 1024 + 64);
    // Lead paragraph differs by tag so the PREFIX diverges immediately —
    // this is what forces set_content_async off its cheap append path and
    // onto the worker (a pure append would stay sync regardless of size).
    s += "# Heading ";
    s += tag;
    s += "\n\nOpening paragraph tagged ";
    s += tag;
    s += ".\n\n";
    int i = 0;
    while (s.size() < static_cast<std::size_t>(kb) * 1024) {
        s += "- item ";
        s += tag;
        s += std::to_string(i++);
        s += " some filler prose to pad the body out to size\n";
    }
    return s;
}

// Poll build() and adopt any landed async result, with a bounded wait, to
// let the widget quiesce. Returns true if the widget became not-parsing
// within the deadline.
bool drain_async(StreamingMarkdown& md, int max_ms = 3000) {
    using clock = std::chrono::steady_clock;
    const auto deadline = clock::now() + std::chrono::milliseconds(max_ms);
    while (clock::now() < deadline) {
        (void)md.build();               // maybe_apply_async_ runs here
        if (!md.is_parsing()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return !md.is_parsing();
}

// ── 1. The BUG: async divergent >= 16 KB freezes the reveal ──────────────
//
// Prove the mechanism the fix removes. Feed body A (settled), then feed a
// DIVERGENT body B >= 16 KB via set_content_async. While the worker runs,
// is_parsing() must be true and source() must still reflect A (the tree
// build() returns is the OLD one). Only after draining does B land.
void async_divergent_over_16k_freezes_reveal() {
    StreamingMarkdown md;
    md.set_live(true);

    const std::string a = big_body(20, 'A');   // > 16 KB
    md.set_content_async(a);
    CHECK(drain_async(md), "body A failed to settle");
    (void)md.build();
    const std::string src_a = std::string{md.source()};
    CHECK(src_a == a, "body A did not become the widget source");

    // Sub-turn boundary: divergent prefix, still > 16 KB. This is the
    // exact change class cached_markdown_for USED to route through
    // set_content_async on the live path.
    const std::string b = big_body(24, 'B');
    CHECK(b.size() >= 16u * 1024, "body B under async threshold — test invalid");
    md.set_content_async(b);

    // The mechanism: a background worker is now in flight. Until it lands
    // AND build() is polled, the widget is frozen on A. Assert BOTH: it
    // is parsing, and its source has NOT advanced to B yet.
    const bool froze = md.is_parsing() && std::string{md.source()} != b;
    CHECK(froze,
          "async divergent >=16KB did NOT freeze — repro invalid, the bug "
          "the fix targets is not being exercised");

    // And it recovers only once build() is polled to adopt the result —
    // the poll that the frozen-scrollback visual-hash gate can suppress
    // in production, which is what stalls the tail.
    CHECK(drain_async(md), "body B never landed");
    (void)md.build();
    CHECK(std::string{md.source()} == b, "body B never adopted after drain");
}

// ── 2. The FIX: sync set_content never freezes ───────────────────────────
//
// The same divergent >= 16 KB change fed via set_content (what the live
// !settled path now uses) must apply IN THE SAME CALL: never is_parsing(),
// and source() reflects B immediately, no build() poll required.
void sync_set_content_never_freezes() {
    StreamingMarkdown md;
    md.set_live(true);

    const std::string a = big_body(20, 'A');
    md.set_content(a);
    CHECK(!md.is_parsing(), "sync set_content spawned a worker (A)");
    CHECK(std::string{md.source()} == a, "sync body A not applied immediately");

    // Divergent sub-turn boundary, > 16 KB — the case that froze async.
    const std::string b = big_body(24, 'B');
    md.set_content(b);

    // The whole point: no worker, no is_parsing() window, content live NOW.
    CHECK(!md.is_parsing(),
          "sync set_content entered is_parsing() on a >=16KB divergent "
          "change — the freeze window is NOT closed");
    CHECK(std::string{md.source()} == b,
          "sync set_content did not reflect B immediately — reveal would "
          "still be stale this frame");

    // One more boundary to be sure it holds across repeated resets.
    const std::string c = big_body(28, 'C');
    md.set_content(c);
    CHECK(!md.is_parsing(), "sync set_content parsed async on 3rd boundary");
    CHECK(std::string{md.source()} == c, "sync body C not applied immediately");
}

} // namespace

int main() {
    std::printf("=== stream_async_freeze_test ===\n");
    async_divergent_over_16k_freezes_reveal();
    sync_set_content_never_freezes();
    std::printf("  %d checks, %d failures\n", g_checks, g_failures);
    if (g_failures == 0) {
        std::printf("PASSED\n");
        return 0;
    }
    std::printf("FAILED\n");
    return 1;
}
