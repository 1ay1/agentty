#pragma once
#include <maya/widget/welcome_screen.hpp>
#include "moha/runtime/model.hpp"

namespace moha::ui {

[[nodiscard]] maya::WelcomeScreen::Config welcome_screen_config(const Model& m);

} // namespace moha::ui
