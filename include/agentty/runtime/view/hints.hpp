// SPDX-License-Identifier: Apache-2.0
#pragma once
// agentty::ui — a responsive key-hint footer strip, shared by the pickers and
// the diff-review pane (any modal with a "key label   key label …" footer).
// Renders on a SINGLE line; when the width can't fit them all, the lowest-
// priority hints drop out (rightmost wins ties) and survivors keep left-to-
// right order. Never wraps. Extracted so every modal footer degrades the same
// way and destructive actions can be tinted consistently.

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <maya/dsl.hpp>
#include "agentty/runtime/view/palette.hpp"   // color tokens (fg, muted…) + fg_of/fg_dim
#include "agentty/runtime/view/helpers.hpp"   // query_caret (filter_header)

namespace agentty::ui {

// One key-binding hint: a key glyph + short label, a priority (higher = kept
// longer under width pressure), and an optional accent colour for the KEY
// glyph (e.g. success for "accept", danger for "reject" — so destructive
// actions never look identical to benign ones).
struct Hint {
    std::string  key;
    std::string  label;
    int          priority = 0;
    maya::Color  key_color = fg;   // default = normal foreground
};

[[nodiscard]] inline maya::Element key_hints(std::vector<Hint> hints) {
    using namespace maya;
    using namespace maya::dsl;
    return component([hints = std::move(hints)](int w, int) -> Element {
        if (w <= 0 || hints.empty()) return nothing();
        constexpr int gap = 3;   // columns between adjacent hints
        auto pair_w = [](const Hint& hn) {
            return string_width(hn.key) + 1 + string_width(hn.label);
        };
        std::vector<bool> keep(hints.size(), true);
        auto total = [&] {
            int sum = 0, shown = 0;
            for (std::size_t i = 0; i < hints.size(); ++i)
                if (keep[i]) { sum += pair_w(hints[i]); ++shown; }
            if (shown > 1) sum += gap * (shown - 1);
            return sum;
        };
        // Greedily evict the lowest-priority kept hint until the strip fits.
        while (total() > w) {
            int victim = -1;
            for (std::size_t i = 0; i < hints.size(); ++i) {
                if (!keep[i]) continue;
                if (victim < 0 || hints[i].priority <=
                        hints[static_cast<std::size_t>(victim)].priority)
                    victim = static_cast<int>(i);
            }
            if (victim < 0) break;
            keep[static_cast<std::size_t>(victim)] = false;
        }
        std::vector<Element> parts;
        bool first = true;
        for (std::size_t i = 0; i < hints.size(); ++i) {
            if (!keep[i]) continue;
            if (!first) parts.push_back(text(std::string(gap, ' ')));
            first = false;
            parts.push_back(text(hints[i].key + " ", fg_of(hints[i].key_color)));
            parts.push_back(text(hints[i].label, fg_dim(muted)));
        }
        if (parts.empty()) return nothing();
        return h(std::move(parts)).build();
    });
}

// The filter line at the top of a picker: a magnifier, then either the
// query or a greyed prompt.
//
// ONE definition because every picker had hand-rolled its own, and they had
// drifted in exactly the ways hand-rolled chrome does: the theme browser led
// with a palette emoji instead of the magnifier and indented its text by an
// extra space, so switching between it and the provider picker moved the
// query line sideways under the cursor. Same widget, three spellings.
//
// `noun` names what is being filtered ("providers", "symbols"); empty gives
// the bare "type to filter…", which is right when the panel title already
// says what the list is.
//
// `caret` adds a blinking insertion point after the query, for the pickers
// that read as a prompt you are typing INTO (the command palette, the model
// picker) rather than a list you are narrowing. Passing a colour opts in;
// the default is no caret.
[[nodiscard]] inline maya::Element filter_header(std::string_view query,
                                                std::string_view noun = {},
                                                std::optional<maya::Color> caret = {},
                                                std::string_view glyph = "\xf0\x9f\x94\x8d ") {
    using namespace maya;
    using namespace maya::dsl;
    std::string prompt = "type to filter";
    if (!noun.empty()) { prompt += ' '; prompt += noun; }
    prompt += "\xe2\x80\xa6";                      // …
    const bool empty_q = query.empty();

    std::vector<Element> parts;
    parts.push_back(text(std::string{glyph},
                         caret ? fg_bold(*caret) : fg_of(muted)));
    if (empty_q) {
        // Caret BEFORE the prompt: it marks where the first keystroke lands,
        // and the prompt is placeholder text sitting to its right.
        if (caret) parts.push_back(query_caret(*caret));
        parts.push_back(text(std::move(prompt), fg_italic(muted)));
    } else {
        parts.push_back(text(std::string{query}, fg_of(fg)));
        if (caret) parts.push_back(query_caret(*caret));
    }
    return h(std::move(parts)).build();
}

} // namespace agentty::ui
