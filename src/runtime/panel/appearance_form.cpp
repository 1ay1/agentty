// appearance_form.cpp — build the Appearance pane's form from the prefs.
//
// A pure projection: prefs in, form::Form out. Every row is DATA, so the
// shared form reducer owns navigation, the dropdowns and the editing, and
// maya::Form owns every glyph. This file's only opinion is which rows exist,
// what they are called, and what each one says about itself.

#include "agentty/runtime/panel/appearance.hpp"

#include "agentty/domain/ui_theme.hpp"

#include <maya/style/schemes.hpp>

#include <array>
#include <cctype>
#include <string>
#include <vector>

namespace agentty::ui::panel {

namespace {

namespace up = agentty::ui_prefs;

// A Choice built from an enum's labels, selected by the current value.
//
// Every one of these enums is small and closed — three to five values that
// fit in a header comment — which is exactly form.hpp's rule for a Choice
// rather than a Pick.
template <typename E, std::size_t N>
void enum_choice(form::Builder& b, std::string_view id, std::string label,
                 const std::array<E, N>& values, E current,
                 std::string help,
                 const std::array<const char*, N>& blurbs) {
    std::vector<std::string> labels, ids, hints;
    labels.reserve(N); ids.reserve(N); hints.reserve(N);
    for (std::size_t i = 0; i < N; ++i) {
        labels.emplace_back(up::label(values[i]));
        ids.emplace_back(up::label(values[i]));
        hints.emplace_back(blurbs[i]);
    }
    b.choice(std::string{id}, std::move(label), std::move(labels),
             std::move(ids), up::label(current), std::move(help),
             std::move(hints));
}

}  // namespace

form::Form build_appearance_form(const up::Prefs& p, bool tty) {
    const up::Resolved r = up::resolve(p, tty);

    form::Builder b{" Appearance "};

    // The subtitle carries the one fact the rows cannot: where these are
    // saved. Appearance is a USER concern — a light terminal is a property
    // of your eyes, not of the repo — and saying so here stops the question
    // being asked of every individual row.
    b.subtitle("how agentty looks \xc2\xb7 saved for your user, not this project");

    // ── Theme ───────────────────────────────────────────────────────
    b.header("Theme");
    {
        // A Pick, not a Choice: 57 schemes and growing, which is precisely
        // the case form.hpp says belongs in a searchable picker rather than
        // a dropdown. The row shows what is IN USE, with the reason when
        // that differs from what was asked for.
        std::string shown = p.theme.empty() ? "native" : p.theme;
        const std::string_view why = up::theme_override_reason(p, r);
        if (!why.empty()) { shown += "  \xe2\x80\x94  "; shown += why; }
        b.pick(std::string{kApTheme}, "Scheme", std::move(shown),
               "native keeps your terminal's own colors \xc2\xb7 Enter to browse");
        if (p.theme.empty())
            b.origin("your terminal");
        else if (!why.empty())
            b.origin("not applied");
    }

    // ── Color ───────────────────────────────────────────────────────
    b.header("Color");
    {
        static constexpr std::array kTiers = {
            up::ColorTier::Auto, up::ColorTier::TrueColor,
            up::ColorTier::Ansi256, up::ColorTier::Ansi16, up::ColorTier::Mono,
        };
        static constexpr std::array<const char*, 5> kTierBlurbs = {
            "detect from the terminal",
            "24-bit \xc2\xb7 38;2;r;g;b",
            "the xterm-256 palette",
            "the sixteen every terminal has",
            "no color at all",
        };
        enum_choice(b, kApTier, "Colors", kTiers, p.tier,
                    "what agentty may emit", kTierBlurbs);
        // The RESOLVED value as provenance. A row reading "auto" alone
        // cannot tell a working default from a detection that has gone
        // wrong on this terminal — which is the whole of issue #37.
        if (p.tier == up::ColorTier::Auto) {
            switch (r.tier) {
                case maya::theme::ColorTier::TrueColor: b.origin("detected: truecolor"); break;
                case maya::theme::ColorTier::Ansi256:   b.origin("detected: 256 colors"); break;
                case maya::theme::ColorTier::Ansi16:    b.origin("detected: 16 colors"); break;
                case maya::theme::ColorTier::Mono:      b.origin("detected: monochrome"); break;
            }
        }
    }
    {
        static constexpr std::array kPol = {
            up::Polarity::Auto, up::Polarity::Dark, up::Polarity::Light,
        };
        static constexpr std::array<const char*, 3> kPolBlurbs = {
            "read COLORFGBG, else leave it unknown",
            "assume a dark background",
            "assume a light background",
        };
        enum_choice(b, kApPolarity, "Background", kPol, p.polarity,
                    "only consulted by schemes that have both", kPolBlurbs);
        if (p.polarity == up::Polarity::Auto) {
            switch (r.polarity) {
                case maya::theme::Polarity::Dark:    b.origin("detected: dark"); break;
                case maya::theme::Polarity::Light:   b.origin("detected: light"); break;
                // Not a failure. Most terminals never set COLORFGBG, and
                // guessing is the bug this whole pane exists to fix.
                case maya::theme::Polarity::Unknown: b.origin("not reported"); break;
            }
        }
    }

    // ── Layout ──────────────────────────────────────────────────────
    b.header("Layout");
    {
        static constexpr std::array kDens = {
            up::Density::Compact, up::Density::Normal, up::Density::Roomy,
        };
        static constexpr std::array<const char*, 3> kDensBlurbs = {
            "10 rows \xc2\xb7 for a short terminal",
            "14 rows \xc2\xb7 the shared default",
            "22 rows \xc2\xb7 fewer panels scroll",
        };
        enum_choice(b, kApDensity, "Panel height", kDens, p.density,
                    "how tall a panel's body may get", kDensBlurbs);
    }
    // A reading measure. 0 is a real value here — "no cap" — so it is a
    // Number rather than a Choice: the useful settings are a continuum
    // (72, 80, 100), not an enum anyone could name.
    b.number(std::string{kApProseWidth}, "Prose width", p.prose_width, 0, 200,
             "wrap assistant text at N columns \xc2\xb7 0 = the full width");
    b.toggle(std::string{kApCompact}, "Compact turns", p.compact_turns,
             "drop the blank line between turns");

    // ── Motion ──────────────────────────────────────────────────────
    b.header("Motion");
    {
        static constexpr std::array kMot = {
            up::Motion::Full, up::Motion::Reduced, up::Motion::Off,
        };
        static constexpr std::array<const char*, 3> kMotBlurbs = {
            "streaming reveal and spinners",
            "no reveal \xc2\xb7 spinners kept",
            "nothing animates",
        };
        // Reduced motion is an accessibility setting before it is a taste:
        // a typewriter reveal is genuinely unpleasant with a vestibular
        // disorder, and a spinner on a slow link is noise on the wire.
        enum_choice(b, kApMotion, "Animation", kMot, p.motion,
                    "reduce or stop movement", kMotBlurbs);
    }

    // ── Content ─────────────────────────────────────────────────────
    b.header("Content");
    b.toggle(std::string{kApSyntax}, "Syntax highlighting", p.syntax,
             "colour code fences by language");
    {
        static constexpr std::array kTool = {
            up::ToolOutput::Collapsed, up::ToolOutput::Preview, up::ToolOutput::Full,
        };
        static constexpr std::array<const char*, 3> kToolBlurbs = {
            "just the header line",
            "the first few lines",
            "everything, always",
        };
        enum_choice(b, kApToolOutput, "Tool output", kTool, p.tool_output,
                    "how much a tool shows before you ask", kToolBlurbs);
    }
    {
        static constexpr std::array kThink = {
            up::Thinking::Shown, up::Thinking::Collapsed, up::Thinking::Hidden,
        };
        static constexpr std::array<const char*, 3> kThinkBlurbs = {
            "reasoning inline",
            "a line you can open",
            "never shown",
        };
        enum_choice(b, kApThinking, "Thinking", kThink, p.thinking,
                    "whether the model's reasoning is shown", kThinkBlurbs);
    }
    {
        static constexpr std::array kStamp = {
            up::Timestamps::Off, up::Timestamps::Relative, up::Timestamps::Absolute,
        };
        static constexpr std::array<const char*, 3> kStampBlurbs = {
            "no times",
            "\"2m ago\"",
            "\"14:32\"",
        };
        enum_choice(b, kApTimestamps, "Timestamps", kStamp, p.timestamps,
                    "whether turns carry a time", kStampBlurbs);
    }

    // No "unsaved" marker and no ^S: every row here writes through the
    // moment it changes, because a theme is judged by looking at it and an
    // apply step between choosing and seeing makes that impossible.
    b.note("changes apply immediately");
    return b.build();
}

// ── Theme search ────────────────────────────────────────────────────────
//
// Subsequence matching, not substring: "gvd" finds "Gruvbox Dark" the way a
// fuzzy picker should, which matters when the list is 57 long and the names
// are two and three words. Case-insensitive, and an empty query lists
// everything so the picker opens as a browsable catalogue rather than a
// blank prompt.
//
// "native" leads the list always. It is not one of the generated schemes —
// it is the absence of one — and the way back to your own terminal colors
// must never be something you have to know the name of.
std::vector<std::string> matching_themes(std::string_view query) {
    auto lower = [](std::string_view s) {
        std::string o; o.reserve(s.size());
        for (char c : s) o.push_back(static_cast<char>(std::tolower(
            static_cast<unsigned char>(c))));
        return o;
    };
    const std::string q = lower(query);

    auto subseq = [](const std::string& hay, const std::string& needle) {
        std::size_t k = 0;
        for (char c : hay) if (k < needle.size() && c == needle[k]) ++k;
        return k == needle.size();
    };

    std::vector<std::string> out;
    // native first, and only when it matches — so a query that clearly means
    // a scheme does not carry it along at the top.
    if (q.empty() || subseq("native", q)) out.emplace_back();

    for (const auto& s : maya::theme::schemes) {
        const std::string name{s.name};
        if (q.empty() || subseq(lower(name), q)) out.push_back(name);
    }
    return out;
}

}  // namespace agentty::ui::panel
