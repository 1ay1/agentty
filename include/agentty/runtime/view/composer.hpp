#pragma once
#include <maya/widget/composer.hpp>
#include "agentty/runtime/model.hpp"

namespace agentty::ui {

[[nodiscard]] maya::Composer::Config composer_config(const Model& m);

// Will the composer claim the terminal's HARDWARE caret this frame?
//
// Extracted from composer_config so the one condition has one definition.
// It decides two things that must agree, in different files:
//
//   * the config we hand maya (composer.cpp), and
//   * whether visual_hash should track a blink phase (program.hpp).
//
// With the hardware caret the terminal owns the blink: maya paints no
// caret cell and schedules no frames, so there is no visible step for the
// hash to follow. A host that mixes a blink parity anyway makes the hash
// flip ~4x/sec on a still screen, and the loop repaints the composer under
// a caret the terminal is blinking on its own clock — they beat, and the
// caret flickers. Re-deriving the condition by hand is what let those two
// drift apart; this is the fix.
[[nodiscard]] bool composer_uses_hardware_caret(const Model& m) noexcept;

} // namespace agentty::ui
