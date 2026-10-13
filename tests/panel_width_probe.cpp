// panel_width_probe — does a wide terminal actually buy wider content?
//
// Reported in #75: a long URL in a form field renders cut off while the
// pane has obvious room to its right, and a long error under a row does
// the same.
//
// Both are one fault with two faces: a width budget derived from
// Panel::Config::min_width instead of from the terminal. min_width is a
// FLOOR — the panel stretches past it to fill its container — so every
// budget computed from it is frozen at the floor's value and answers the
// same number on a 60-column pane and a 200-column one. draw_budget()
// already learned this (its comment says so at length); edit_budget()
// did not.
//
// The measurement is end-to-end on a real canvas: render the pane at
// several widths and count how many characters of the value survived.
// A budget that tracks the terminal grows that count; a frozen one does
// not.

#include "agtest.hpp"

#include <maya/print.hpp>
#include <maya/widget/panel.hpp>

#include <cstdlib>
#include <string>

namespace {

// The panel reads the terminal width through COLUMNS when there is no tty,
// which is every test. Set it for one render and put it back — a leaked
// COLUMNS changes what every later panel test measures, which is how this
// probe first broke two of its neighbours.
struct AtWidth {
    std::string saved;
    bool had = false;
    explicit AtWidth(int w) {
        if (const char* c = std::getenv("COLUMNS")) { saved = c; had = true; }
        setenv("COLUMNS", std::to_string(w).c_str(), /*overwrite=*/1);
    }
    ~AtWidth() {
        if (had) setenv("COLUMNS", saved.c_str(), /*overwrite=*/1);
        else     unsetenv("COLUMNS");
    }
};

// A URL long enough to overflow any of these widths, with no repeating
// run — so "how much of it is on screen" can be read off the last digit
// that survived rather than inferred.
const std::string kUrl =
    "https://gateway.example.com/v1/openai/compat/deployments/"
    "gpt-4o-mini-2024-07-18/chat/completions?api-version=2024-08-01";

// Longest run of the URL present on any row, in characters.
std::size_t url_chars_visible(int w, bool editing) {
    const AtWidth at{w};

    maya::Panel::Config cfg;
    cfg.title     = " Plugin ";
    cfg.min_width = 60;          // kPanelStandard, the usual floor

    maya::Panel::Item row;
    row.leading = "URL";
    row.control = maya::panel::Text{
        kUrl, editing ? kUrl.size() : std::string::npos, {}};
    cfg.items.push_back(std::move(row));

    // render_to_string lays the tree out at `w`, and COLUMNS tells the
    // panel that is the terminal — the two have to agree, or the budget is
    // about a different width than the one being painted.
    const std::string out =
        maya::render_to_string(maya::Panel{std::move(cfg)}.build(), w);

    // Longest stretch of URL-ish characters on any line. The caret window
    // splices ‹ › markers around its slice, so searching for the whole URL
    // never matches — what matters is how WIDE the surviving slice is.
    std::size_t best = 0;
    std::size_t run  = 0;
    for (const char c : out) {
        const bool urlish = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
                         || (c >= '0' && c <= '9')
                         || c == '/' || c == '.' || c == ':'
                         || c == '-' || c == '?' || c == '=';
        if (urlish) { if (++run > best) best = run; }
        else run = 0;
    }
    return best;
}

}  // namespace

TEST_CASE("panel: a wider terminal shows more of a long value") {
    // The pane's floor is 60. At 80 columns there is already slack; by
    // 200 there is three times the floor. Every step should show more.
    const std::size_t at80  = url_chars_visible(80,  /*editing=*/true);
    const std::size_t at120 = url_chars_visible(120, /*editing=*/true);
    const std::size_t at200 = url_chars_visible(200, /*editing=*/true);

    INFO("visible url chars: 80=", at80, " 120=", at120, " 200=", at200);

    // Not an exact count — the window splices scroll markers and the
    // label lane varies. The claim is monotone growth: width the pane
    // was given must reach the value.
    CHECK(at120 > at80);
    CHECK(at200 > at120);

    // And the growth must be real, not a column or two of rounding. At
    // 200 columns a 120-character URL should be entirely on screen.
    CHECK(at200 >= 110);
}

TEST_CASE("panel: an idle long value is not windowed to the floor either") {
    const std::size_t at80  = url_chars_visible(80,  /*editing=*/false);
    const std::size_t at200 = url_chars_visible(200, /*editing=*/false);
    INFO("idle url chars: 80=", at80, " 200=", at200);
    CHECK(at200 > at80);
}

TEST_CASE("panel: a long row error wraps instead of losing its tail") {
    // The second screenshot on #75: the error under a row is cut mid-word
    // with an ellipsis while the rows below it are blank. An error is the
    // one string in a pane that must arrive whole — it is the only thing
    // telling the user what to fix.
    const std::string err =
        "connect failed: no route to host 10.42.0.17:11434 "
        "(check the endpoint, or whether the server is listening)";

    const AtWidth at{100};

    maya::Panel::Config cfg;
    cfg.title     = " Plugin ";
    cfg.min_width = 60;
    maya::Panel::Item row;
    row.leading = "Endpoint";
    row.control = maya::panel::Text{"10.42.0.17:11434", std::string::npos, {}};
    row.error   = err;
    cfg.items.push_back(std::move(row));

    const std::string out =
        maya::render_to_string(maya::Panel{std::move(cfg)}.build(), 100);

    // The tail is the part that tells you what to do about it.
    CHECK(out.find("server is listening)") != std::string::npos);
}

TEST_CASE("panel: a pane-wide error wraps where a subtitle would truncate") {
    // Config::error exists because hosts were putting failures in
    // `subtitle`, which truncates — so the half of the message naming the
    // fix never arrived.
    const AtWidth at{70};

    maya::Panel::Config cfg;
    cfg.title     = " Plugin ";
    cfg.min_width = 60;
    cfg.subtitle  = "not connected";
    cfg.error     = "spawn failed: no such file or directory "
                    "(is mcp-server-git on PATH?)";
    cfg.items.push_back(maya::Panel::Item{});

    const std::string out =
        maya::render_to_string(maya::Panel{std::move(cfg)}.build(), 70);

    CHECK(out.find("not connected") != std::string::npos);
    CHECK(out.find("on PATH?)") != std::string::npos);
}
