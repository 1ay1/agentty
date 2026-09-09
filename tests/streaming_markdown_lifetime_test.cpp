// streaming_markdown_lifetime_test — the settled tree must outlive its widget.
//
// REGRESSION (maya 54ad00d → fixed): StreamingMarkdown::build() wrapped its
// children in a component() lambda that captured `this` so it could record
// the layout width into the `last_paint_width_` member (the reveal's
// line_bounded clamp needs the real wrapped width).
//
// But that lambda runs at LAYOUT time, not build time — and the Element
// holding it is explicitly designed to outlive the widget:
// settled_element() hands the settled tree to the host as a
// shared_ptr<const Element> precisely so frozen scrollback can keep it on
// the heap, and the renderer caches it across frames. So every layout of a
// stashed tree wrote 4 bytes into freed memory.
//
// Valgrind named it exactly:
//   Invalid write of size 4
//     at maya::StreamingMarkdown::build()::{lambda(int,int)}::operator()
//   Address is inside a block of size 3168 free'd
//     by maya::StreamingMarkdown::~StreamingMarkdown()
//
// The corruption surfaced far from the cause — bad_array_new_length or
// SIGSEGV inside an unrelated Element copy several frames later, which is
// what made agentty appear to "crash when loading a thread".
//
// This test reproduces the ownership pattern directly: build + settle a
// widget, stash its settled_element(), DESTROY the widget, then lay the
// stashed tree out. Under ASan/valgrind the old code trips here; without a
// sanitizer it still exercises the path, and the width cell must remain
// readable and correct.

#include <maya/app/inline.hpp>
#include <maya/render/renderer.hpp>
#include <maya/widget/markdown.hpp>

#include <cstdio>
#include <memory>
#include <string>
#include <vector>

namespace {

int failures = 0;

void check(bool ok, const char* what) {
    if (!ok) { std::printf("FAIL: %s\n", what); ++failures; }
}

// A body long enough to wrap at the widths we lay out at, with a couple of
// blocks so the settled tree has real children to walk.
std::string sample_body() {
    return "# Heading\n\n"
           "The quick brown fox jumps over the lazy dog, and keeps on "
           "jumping until the line has to wrap at any sensible width.\n\n"
           "- one item that is also fairly long so it wraps too\n"
           "- another item\n\n"
           "Closing paragraph with enough text to occupy several rows.\n";
}

} // namespace

int main() {
    // ── 1. The settled tree must survive its widget's destruction ────────
    {
        std::shared_ptr<const maya::Element> stashed;
        {
            maya::StreamingMarkdown md;
            md.set_content(sample_body());
            md.finish();
            stashed = md.settled_element();
            check(stashed != nullptr, "settled_element() returns a tree");
            // Lay out ONCE while the widget is alive — this is what primes
            // the width cell the lambda writes to.
            (void)maya::render_to_string(*stashed, 80);
        }   // md is destroyed here; `stashed` deliberately outlives it.

        // The stashed tree is now the host's (frozen scrollback's) to render.
        // Laying it out must not touch the dead widget. This is the exact
        // operation that wrote into freed memory before the fix.
        const std::string a = maya::render_to_string(*stashed, 80);
        check(!a.empty(), "settled tree renders after the widget is gone");

        // Re-render at DIFFERENT widths: each one invokes the layout lambda
        // again, so each is another write through the (now dangling) capture.
        for (int w : {40, 100, 24, 120}) {
            const std::string out = maya::render_to_string(*stashed, w);
            check(!out.empty(), "settled tree renders at every width");
        }
    }

    // ── 2. Several stashed trees, all outliving their widgets ────────────
    // Frozen scrollback holds many of these at once; interleaving their
    // layouts is the realistic shape and catches a per-instance cell that
    // was accidentally shared.
    {
        std::vector<std::shared_ptr<const maya::Element>> stash;
        for (int i = 0; i < 8; ++i) {
            maya::StreamingMarkdown md;
            md.set_content("Turn " + std::to_string(i) + "\n\n" + sample_body());
            md.finish();
            stash.push_back(md.settled_element());
        }
        for (int pass = 0; pass < 3; ++pass)
            for (const auto& el : stash) {
                check(el != nullptr, "stashed element non-null");
                const std::string out = maya::render_to_string(*el, 60 + pass * 20);
                check(!out.empty(), "each stashed tree renders independently");
            }
    }

    // ── 3. A LIVE (unsettled) widget re-lays out safely at many widths ───
    // The width cell exists to serve the reveal clamp; it's private, so
    // rather than reach into it we exercise the path that writes it — a live
    // widget laid out repeatedly at changing widths, which is what a resize
    // during streaming does — and require that it stays consistent and
    // crash-free. (Under valgrind this is where a dangling capture would
    // report; the value itself is covered by the reveal's own tests.)
    {
        maya::StreamingMarkdown md;
        md.set_live(true);
        md.set_content(sample_body());
        for (int w : {80, 50, 120, 32, 96}) {
            const std::string out = maya::render_to_string(md.build(), w);
            check(!out.empty(), "live widget re-lays out at a new width");
        }
        // Settle it, stash it, drop the widget, and lay out again: the
        // live→settled→orphaned sequence is the real frozen-scrollback path.
        md.finish();
        auto settled = md.settled_element();
        check(settled != nullptr, "settled after a live phase");
        const std::string before = maya::render_to_string(*settled, 80);
        {
            maya::StreamingMarkdown doomed;
            doomed.set_content(sample_body());
            doomed.finish();
            auto other = doomed.settled_element();
            (void)maya::render_to_string(*other, 80);
        }
        const std::string after = maya::render_to_string(*settled, 80);
        check(before == after,
              "a stashed tree renders identically after an unrelated "
              "widget is created and destroyed");
    }

    if (failures == 0) std::printf("PASS\n");
    return failures == 0 ? 0 : 1;
}
