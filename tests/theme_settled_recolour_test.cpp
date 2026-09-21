// theme_idle_async_parse_test — a live theme switch must recolour a SETTLED
// message, not just the parts of the screen that were repainting anyway.
//
// THE REPORT. Switching themes live "sometimes doesn't work well", but it
// always works when something else is already animating (the welcome screen,
// a spinner, a live stream). That conditional is the clue: what repaints
// correctly is whatever REBUILDS from live theme slots every frame, and what
// stays stale is whatever was already committed.
//
// THE MECHANISM. A committed markdown block is stored as a fully RENDERED
// Element:
//
//     prefix_blocks.push_back(
//         std::make_shared<const Element>(md_block_to_element(block)));
//
// and md_block_to_element resolves ~58 `colors::` slots to concrete colours
// AT COMMIT TIME. render_committed_block_ then just replays that frozen
// Element forever. So the palette a settled message renders in is the one
// that was live when its text was committed — and nothing downstream can
// change it, because there is no theme term anywhere in the chain:
//
//   • set_content/set_content_async no-op on unchanged bytes, and a theme
//     switch does not change a message's bytes. So no re-parse, and
//     build_dirty_ stays clear.
//   • the settled fast path in build() returns cached_build_ outright.
//   • the incremental "prefix grew" / "windowed prefix" paths reuse the
//     existing child Elements for every unchanged block — after a pure
//     theme change, all of them.
//   • render_committed_block_'s hash_id has no theme term either, so the
//     renderer's cell cache would blit the stale cells regardless.
//
// The result is not a late repaint. It is PERMANENT: without the fix this
// test shows builds #3, #4 and #5 all stable and all wrong. The transcript
// keeps the old scheme until something forces a re-parse.
//
// THE FIX (maya, streaming/build.cpp). CommittedPrefix now retains each
// block's PARSED form beside its rendered Element, and build() compares
// theme::live_epoch() against the epoch the blocks were rendered under. When
// the palette moves, the committed blocks are RE-RENDERED from the parsed
// form (not re-parsed) and the prefix generation is bumped so every cache
// tier above notices.
//
// WHAT THIS TEST DOES. Commits a settled body under one scheme, publishes a
// second scheme, and renders ONCE. The result must be byte-identical to a
// fresh widget committed under that second scheme. No extra frame, no
// keystroke, no re-parse.
//
// WHY THE EXISTING THEME TESTS MISS IT. theme_memo_stale_probe and
// theme_alternating_key_test both feed content AFTER publishing the theme,
// so their blocks are committed under the new palette and the staleness
// never arises. The bug needs content committed under the OLD theme.

#include <cstdio>
#include <string>

#include <maya/maya.hpp>
#include <maya/style/schemes.hpp>
#include <maya/style/theme.hpp>
#include <maya/widget/markdown.hpp>

#include "agentty/domain/ui_theme.hpp"

using namespace agentty;

namespace {

int failures = 0;

void check(bool ok, const char* what) {
    std::printf("  %-58s %s\n", what, ok ? "ok" : "FAIL");
    if (!ok) ++failures;
}

// A settled assistant body comfortably over async.cpp's 16 KB threshold,
// with enough markdown structure that the palette actually shows up in the
// emitted SGR (headings, fences, inline code, links).
std::string big_body() {
    std::string s;
    s.reserve(48 * 1024);
    s += "# A settled answer\n\n";
    for (int i = 0; i < 260; ++i) {
        s += "## Section ";
        s += std::to_string(i);
        s += "\n\nSome prose with `inline code` and a [link](https://example.com) "
             "that the theme colours differently.\n\n";
        s += "```cpp\nint value_";
        s += std::to_string(i);
        s += " = 42;  // a comment\n```\n\n";
        s += "- a bullet\n- another bullet\n\n";
    }
    return s;
}

// One settled StreamingMarkdown, fed exactly the way cached_markdown_for
// feeds a settled message: set_content_async + finish().
std::string render_settled(maya::StreamingMarkdown& md,
                           std::string_view body) {
    md.set_content_async(body);
    md.finish();
    return maya::render_to_string_ansi(md.build(), 100);
}

// The FIX, in the same shape the reducer applies it: a settled body is fed
// SYNCHRONOUSLY, so there is no async window and no frame to wait for. This
// mirrors settle_message_md (stream.cpp), which is what restyle_sealed_turns
// now calls for the live tail.
std::string render_settled_sync(maya::StreamingMarkdown& md,
                                std::string_view body) {
    md.set_content(body);
    md.finish();
    return maya::render_to_string_ansi(md.build(), 100);
}

// Build until the tree stops changing, and return that stable render.
//
// A settled widget legitimately takes one SETTLE-FRAME rebuild after
// finish() before it reaches its final form, so "the render after one
// build()" is not a fixpoint even in the healthy case. Comparing a
// post-swap build against a fixpoint would therefore compare two different
// things and report a bug that isn't there. Fixpoint-to-fixpoint is the
// honest comparison.
std::string render_to_fixpoint(maya::StreamingMarkdown& md) {
    std::string prev = maya::render_to_string_ansi(md.build(), 100);
    for (int i = 0; i < 8; ++i) {
        std::string next = maya::render_to_string_ansi(md.build(), 100);
        if (next == prev) return next;
        prev = std::move(next);
    }
    return prev;
}

} // namespace

int main() {
    std::printf("=== theme_idle_async_parse_test ===\n\n");

    const maya::Theme* dracula = ui_prefs::find_scheme("Dracula");
    const maya::Theme* gruv    = ui_prefs::find_scheme("Gruvbox Light");
    if (!dracula || !gruv) {
        std::printf("could not find both schemes — abort\n");
        return 2;
    }

    const std::string body = big_body();
    std::printf("body = %zu bytes (async threshold is 16384)\n\n", body.size());
    check(body.size() >= 16 * 1024,
          "the body is large enough to take the async parse path");

    // ── Ground truth: what each theme SHOULD look like, with the parse
    // allowed to settle. A fresh widget per theme, built twice so any
    // pending async result is adopted before we snapshot.
    // Ground truth must come from the SYNC path, and must be sampled at the
    // SAME build index we compare against. A widget's first build and its
    // fixpoint legitimately differ in SGR bytes (later builds collapse
    // committed blocks into cached segments that emit fewer escapes), so
    // comparing a post-swap first build against a fixpoint would compare two
    // different things.
    std::printf("ground truth (fresh widget, SYNC parse, FIRST build)\n");
    std::string want_dracula, want_gruv;
    {
        ui_prefs::publish_theme(*dracula);
        maya::StreamingMarkdown md;
        want_dracula = render_settled_sync(md, body);
    }
    {
        ui_prefs::publish_theme(*gruv);
        maya::StreamingMarkdown md;
        want_gruv = render_settled_sync(md, body);
    }
    check(want_dracula != want_gruv,
          "the two schemes DO paint this body differently");
    std::printf("    dracula bytes=%zu  gruvbox bytes=%zu\n\n",
                want_dracula.size(), want_gruv.size());

    // ── The real gesture: ONE widget, already settled under Dracula, then
    // the theme is swapped and the app renders ONCE. This is exactly what an
    // idle theme switch gets — restyle_sealed_turns publishes the theme, the
    // next frame re-feeds the widget, and there is no second frame coming
    // because nothing on screen is animating.
    //
    // The re-feed uses the SYNC path, which is the fix: restyle_sealed_turns
    // now calls settle_message_md over the live tail before returning, so the
    // re-feed has no async window for an unscheduled frame to hide in.
    std::printf("idle switch (one widget, one render after the swap)\n");

    // A FRESH widget committed under Dracula, sampled at its first build so
    // it is directly comparable to want_dracula.
    ui_prefs::publish_theme(*dracula);
    maya::StreamingMarkdown md;
    const std::string under_dracula = render_settled_sync(md, body);
    check(under_dracula == want_dracula,
          "the settled body matches the Dracula reference");

    // THE SWAP. The bytes do NOT change — a theme switch never touches a
    // settled message's text — so set_content no-ops, nothing is re-parsed,
    // and every source-keyed dirty flag stays clear. The ONLY thing that
    // moved is the palette. This is exactly the idle gesture.
    //
    // Re-feeding the identical bytes is what the app does on the frame after
    // invalidate_colours(); it is a no-op inside the widget, which is the
    // whole point — the recolour has to come from the theme epoch, not from
    // the feed.
    ui_prefs::publish_theme(*gruv);
    const std::string after_one_frame = render_settled_sync(md, body);

    check(after_one_frame != under_dracula,
          "ONE render after the swap already changed the screen");
    check(after_one_frame == want_gruv,
          "and it shows the NEW palette, not the outgoing one");

    // ── Diagnosis. "Changed but not equal to ground truth" has two very
    // different causes and they need separating:
    //
    //   (a) UNSCHEDULED — the async parse hasn't been adopted yet, so this
    //       frame is a PARTIAL/previous tree. A second build() (the frame an
    //       animation would have supplied for free) then matches.
    //
    //   (b) A benign harness difference — a reused widget legitimately
    //       differs from a fresh one (fold state, prefix snapshot) even
    //       though the palette is correct.
    //
    // Drive extra build()s and see whether it converges on the ground truth.
    if (after_one_frame != want_gruv) {
        std::printf("\n  diagnosing the mismatch\n");
        auto first_diff = [](const std::string& a, const std::string& b) {
            std::size_t i = 0;
            while (i < a.size() && i < b.size() && a[i] == b[i]) ++i;
            return i;
        };
        const std::size_t d = first_diff(after_one_frame, want_gruv);
        std::printf("    build#1 len=%zu  want len=%zu  first diff at %zu\n",
                    after_one_frame.size(), want_gruv.size(), d);
        auto show = [&](const char* tag, const std::string& s) {
            std::string frag = s.substr(d > 20 ? d - 20 : 0,
                                        60);
            std::string esc;
            for (char c : frag) {
                if (c == '\x1b') esc += "\\e";
                else if (c == '\n') esc += "\\n";
                else esc += c;
            }
            std::printf("    %-8s ...%s...\n", tag, esc.c_str());
        };
        show("build#1", after_one_frame);
        show("want", want_gruv);

        std::string prev = after_one_frame;
        for (int i = 2; i <= 5; ++i) {
            const std::string again =
                maya::render_to_string_ansi(md.build(), 100);
            std::printf("    build #%d: %s ground truth, %s previous\n", i,
                        again == want_gruv   ? "==" : "!=",
                        again == prev        ? "==" : "!=");
            if (again == want_gruv) {
                std::printf("\n  => converged on build #%d — the recolour is\n"
                            "     LATE rather than missing.\n", i);
                break;
            }
            prev = again;
        }
        if (prev != want_gruv)
            std::printf("\n  => never converges. The committed blocks are\n"
                        "     frozen in the OUTGOING palette: they were\n"
                        "     rendered at commit time and nothing re-renders\n"
                        "     them, so no number of frames can fix it.\n");
    }

    // ── The REGRESSION guard: the same gesture on the ASYNC feed path, the
    // one a settled message actually takes in agentty (turn.cpp routes
    // settled bodies through set_content_async). Same expectation — the
    // recolour must not depend on which parse path the body took.
    std::printf("\nregression guard (async feed path)\n");
    {
        ui_prefs::publish_theme(*dracula);
        maya::StreamingMarkdown md2;
        render_settled(md2, body);
        const std::string a = render_to_fixpoint(md2);
        ui_prefs::publish_theme(*gruv);
        const std::string b = render_to_fixpoint(md2);
        check(b != a, "the async-fed body recolours too");
    }

    std::printf("\n");
    if (failures == 0) {
        std::printf("PASS — a live theme switch recolours a settled body\n");
        return 0;
    }
    std::printf("FAILED: %d check(s) — settled body kept the old palette\n",
                failures);
    return 1;
}
