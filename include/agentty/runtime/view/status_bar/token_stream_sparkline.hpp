#pragma once
#include <maya/widget/token_stream_sparkline.hpp>
#include "agentty/runtime/model.hpp"

namespace agentty::ui {

[[nodiscard]] maya::TokenStreamSparkline::Config
    token_stream_sparkline_config(const Model& m);

} // namespace agentty::ui
