#pragma once
// agentty::ui — small pure helpers shared by view modules.

#include <chrono>
#include <string>
#include <string_view>

#include <maya/style/color.hpp>
#include <maya/style/style.hpp>
#include <maya/dsl.hpp>
#include <maya/terminal/ansi.hpp>

#include "agentty/runtime/model.hpp"

namespace agentty::ui {

// Enum reflection — delegates to agentty::to_string().
[[nodiscard]] inline std::string_view profile_label(Profile p) noexcept { return to_string(p); }
[[nodiscard]] maya::Color profile_color(Profile p) noexcept;
[[nodiscard]] inline std::string_view phase_label(const Phase& p) noexcept { return to_string(p); }

// Status-bar styling — glyph + verb form for the current phase, and a
// terminal color picked to communicate urgency at a glance.
[[nodiscard]] std::string_view phase_glyph(const Phase& p) noexcept;
[[nodiscard]] std::string_view phase_verb(const Phase& p) noexcept;
[[nodiscard]] maya::Color      phase_color(const Phase& p) noexcept;

[[nodiscard]] std::string timestamp_hh_mm(std::chrono::system_clock::time_point tp);

// Full timestamp — "Mon DD HH:MM" (e.g. "Jan 14 09:15") for picker rows.
// Year is omitted on the assumption every visible row is from this year;
// add it via `%Y %b %d %H:%M` if/when threads exceed a year-long history
// becomes a real concern.
[[nodiscard]] std::string timestamp_full(std::chrono::system_clock::time_point tp);

// ── Typographic primitives ────────────────────────────────────────────────
// Letter-spaced uppercase ("S E C T I O N") — the typographic shorthand
// for "this is a section header" in CLI tools that lack real small-caps.
// ASCII-only; non-ASCII bytes pass through unchanged. Only useful for
// short labels — long strings get wide fast (each char gains a space).
[[nodiscard]] std::string small_caps(std::string_view s);

// Right-aligned fixed-width integer. Use for any number that updates
// in place (token counts, durations, pcts, counters) so the surrounding
// row never dances horizontally. Falls back to natural width if `n` is
// wider than `width`.
[[nodiscard]] std::string tabular_int(int n, int width);

// Compact, ALWAYS-5-display-column elapsed-time formatter. Picks the
// best unit for the magnitude:
//     0.0–9.9 s  →  " 4.2s"   (leading space)
//   10.0–99.9 s  →  "12.3s"
//      100–599 s →  " 234s"   (whole seconds)
//        ≥600 s  →  " 9m05s"  (m/s)
//        ≥3600 s →  " >1hr"
// Stable width is the whole point — drop into any always-on indicator
// (phase chip elapsed, tool duration, etc.) and the surrounding layout
// will not shift as the value ticks.
[[nodiscard]] std::string format_elapsed_5(float secs);

// Variable-width compact elapsed-time formatter for non-fixed-width
// surfaces (turn meta, timeline title/footer):
//   < 1 s  → "234ms"
//   < 60 s → "4.2s"
//   else   → "1m20s"
// Use this in any non-status-bar surface where exact width isn't
// required and "234ms" / "4.2s" reads better than padded forms.
[[nodiscard]] std::string format_duration_compact(float secs);

// Normalize an arbitrary model id into a short, human label.
//
// THIN DELEGATE over the domain SSOT — `model_name::decode(id).full()`.
// Kept as a named helper because it is the vocabulary the view layer reads
// in, but it holds no logic of its own: the family table, the version
// extraction, the date/`:latest`/`[1m]` handling and the colour policy all
// live in `domain/model_name.hpp`, proven there and shared with the turn
// header, the status chip and the welcome screen. Do NOT add a special case
// here — add it to the decoder, where every surface gets it.
//
//   codellama:latest        → "Codellama"
//   qwen2.5-coder:7b        → "Qwen2.5 Coder 7b"
//   openai/gpt-4o-mini      → "GPT 4o Mini"
//   claude-sonnet-4-5[1m]   → "Sonnet 4.5 · 1M"
//
// Note the last line: the model name carries NO vendor prefix ("Claude") and
// the `[1m]` marker becomes a visible annotation instead of being silently
// dropped. Provider identity is rendered once, from the registry row, by the
// provider chip — see model_name.hpp on why vendor and provider are
// independent axes that must not be conflated.
[[nodiscard]] std::string pretty_model_label(std::string_view model_id);

// THE canonical human label for a model row — used by the per-provider
// picker, the fused "all providers" picker, AND their fuzzy-match anchor, so
// what you SEE is what gets matched and highlighted, and every provider
// renders identically in both views.
//
// Also a thin delegate: `model_name::decode(id, display_name).full()`.
//
// The two label sources are provably inconsistent: `id` is structured and
// uniform ("gpt-5.3-chat-latest"), but the server `display_name` ranges from
// clean ("Claude Sonnet 4.5") through raw-cased ("Hy-MT2-30B-A3B",
// "gpt-image-1.5") to cruft-bearing ("GPT-5.3 Chat (latest)") — and is
// sometimes a marketing alias unrelated to the id ("Nano Banana Pro"). The
// decoder normalizes whichever source it trusts, preferring the id-derived
// form unless the server name carries signal the id cannot reconstruct.
[[nodiscard]] std::string model_display_label(std::string_view id,
                                              std::string_view display_name);

// Context window size for a given model id. Defaults to 200 K but bumps
// to 1 M when the model id carries the agentty-internal `[1m]` tag (which
// triggers the `context-1m-2025-08-07` beta on the wire). Used by the
// status-bar ctx % calculation so the bar doesn't read "180 %" after
// switching to a 1 M-window model with the old 200 K cap baked in.
// The rungs ^W steps through in the model picker. 0 == "auto" (no override;
// fall back to what the provider advertised or the id implies), and it is
// deliberately FIRST so the cycle starts from "let agentty decide" and one
// full lap always returns there.
//
// The sizes are the ones gateways actually serve rather than round numbers
// for their own sake: 32k/64k local builds, 128k the common vLLM and
// OpenAI-compat default, 200k Claude-class, 272k Codex, 400k GPT-5-class,
// 1M the wide tier LiteLLM users hit most often, 2M the Gemini ceiling.
inline constexpr int kContextLadder[] = {
    0, 32'000, 64'000, 128'000, 200'000, 272'000, 400'000, 1'000'000, 2'000'000,
};

// A human label for a window, matching the picker's own formatting
// ("auto", "128k", "1M").
[[nodiscard]] std::string context_window_label(int tokens);

// The window assumed when NOTHING is known about a model — no user
// override, no advertised figure, no recognised id family.
//
// 200k is deliberately conservative rather than optimistic. The two failure
// modes are not symmetric: guessing too HIGH lets the prefix grow past what
// the endpoint accepts and the turn dies on the wire mid-conversation,
// while guessing too LOW only compacts earlier than strictly necessary. So
// the unknown case takes the recoverable error.
inline constexpr int kDefaultContextWindow = 200'000;

// ── Context window resolution ──────────────────────────────────
//
// The window drives the ctx-% readout, the auto-compaction trigger and the
// compaction slice ceiling, so getting it wrong is expensive in both
// directions: too small silently truncates a model that could hold the whole
// transcript, too large lets the prefix run past what the endpoint accepts
// and the turn fails on the wire.
//
// FOUR sources, most-specific first. The layering is the whole design — a
// single source cannot be right for every deployment:
//
//   1. the user's per-model override         (they configured the gateway)
//   2. what the provider ADVERTISED          (probed /api/show, or the
//                                             window in a /v1/models row)
//   3. what the model id implies             (Claude/GPT families, `[1m]`)
//   4. the conservative default              (nothing is known)
//
// Note the order of 2 and 3: a LIVE figure from the endpoint that will serve
// the request outranks a guess from the id. The same name behind two
// gateways can be served with two different windows, and only the gateway
// knows which.
//
// `advertised` is 0 when nothing was reported — which is why ModelInfo's
// context_window defaults to 0 rather than a number: "unknown" and "200k"
// must not be the same value, or a real 1M model gets clamped by a default
// nobody chose.
[[nodiscard]] int resolve_context_window(std::string_view provider_id,
                                        std::string_view model_id,
                                        int advertised,
                                        const store::Settings& settings) noexcept;

// The composite key an override is stored under. Exposed so the settings
// writer and the resolver cannot disagree about the spelling.
[[nodiscard]] std::string context_override_key(std::string_view provider_id,
                                               std::string_view model_id);

[[nodiscard]] int context_max_for_model(std::string_view model_id) noexcept;

// UTF-8 helpers.
[[nodiscard]] std::string utf8_encode(char32_t cp);
[[nodiscard]] int utf8_prev(std::string_view s, int byte_pos) noexcept;
[[nodiscard]] int utf8_next(std::string_view s, int byte_pos) noexcept;

// Chip-aware variants: same as utf8_prev/utf8_next, but treat any
// composer attachment placeholder (\x01ATT:N\x01) as a single
// navigation unit. Cursor entering the closing sentinel from the
// right jumps to before the opening sentinel; entering the opening
// sentinel from the left jumps to after the closing sentinel.
[[nodiscard]] int chip_prev(std::string_view s, int byte_pos) noexcept;
[[nodiscard]] int chip_next(std::string_view s, int byte_pos) noexcept;

// ── Model list ordering ──────────────────────────────────────
// Providers hand back models in catalog/wire order — effectively random
// to a reader. `model_order_less(a, b)` gives the tidy, human ordering
// the fused "all providers" view already has, for an unqueried
// single-provider list. It is NOT plain lexical sort: version numbers
// break that ("claude-opus-4-10" sorts BEFORE "...-4-8" because '1' <
// '8'; "gpt-10" before "gpt-2"). Instead it is a NATURAL comparator on
// the shown label:
//   1. group by FAMILY — the label's leading non-numeric run ("Claude
//      Sonnet", "GPT"), case-folded — so all Sonnets / all GPT-4s sit
//      together, matching the categorized look of the all-providers view;
//   2. within a family, NEWEST FIRST — numeric chunks compared as
//      integers, descending, so 4.6 ▷ 4.5 ▷ 4 and 10 ▷ 2 (the intent
//      behind every "sort models by release" request — current model on
//      top — achieved without a release date, which local/custom
//      endpoints don't carry);
//   3. full case-folded label as the final, stable tiebreak.
// `a`/`b` are the labels as SHOWN (pretty_model_label output or a
// provider display_name). Favourites are hoisted by the caller BEFORE
// this runs — a deliberate user signal outranks recency.
[[nodiscard]] bool model_order_less(std::string_view a,
                                    std::string_view b) noexcept;

// Hardware-caret cell for an overlay's live search input (command
// palette / model picker / fused picker query lines). One concealed
// █ whose style carries the caret_anchor meta-bit: maya's inline
// serializer moves the REAL terminal cursor onto it and shows it as a
// blinking bar — so the caret travels with focus (composer → palette →
// back) instead of vanishing whenever an overlay opens. IME candidate
// windows anchor in the search field too. Append it directly after
// the query text. `accent` colors the caret via OSC 12 (Rgb only;
// ignored elsewhere — harmless).
// Bottom-most-anchor-wins in the serializer, and the composer stops
// emitting its anchor while any overlay is open (panel::top gate in
// composer_config), so exactly one anchor exists per frame.
//
// Shape 1 (BLINKING block) is deliberate and unconditional: a search
// field is an idle text input, so its caret blinks exactly like the
// composer's idle caret — same everywhere, including under tmux, and
// regardless of the terminal's own default cursor style. The blink is
// the TERMINAL's (DECSCUSR), so it costs zero animation frames. No
// tmux special-case: the epilogue parks at the right margin and resets
// cosmetics, so tmux's copy-mode cursor lands on blank padding rather
// than mid-content (see maya's emit_caret_epilogue). One code path,
// one look, everywhere.
[[nodiscard]] inline auto query_caret(maya::Color accent) {
    return maya::dsl::text("\xe2\x96\x88",
                           maya::Style{}
                               .with_conceal()
                               .with_caret_anchor()
                               .with_caret_shape(1)   // blinking block
                               .with_fg(accent));
}

} // namespace agentty::ui
