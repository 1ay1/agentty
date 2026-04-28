#pragma once
#include <maya/widget/token_stream_sparkline.hpp>
#include "moha/runtime/model.hpp"

namespace moha::ui {

[[nodiscard]] maya::TokenStreamSparkline::Config
    token_stream_sparkline_config(const Model& m);

} // namespace moha::ui
