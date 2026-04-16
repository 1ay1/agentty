#pragma once

#include <maya/maya.hpp>

#include "moha/model.hpp"

namespace moha::views {

// Top-level composition — returns the root maya Element.
maya::dsl::Element root(const Model& m, maya::Ctx ctx);

// Panels
maya::dsl::Element header(const Model& m);
maya::dsl::Element thread_panel(const Model& m, maya::Ctx ctx);
maya::dsl::Element changes_strip(const Model& m);
maya::dsl::Element composer(const Model& m);
maya::dsl::Element status_bar(const Model& m);

// Message & tool-call rendering
maya::dsl::Element render_message(const Message& msg, const Model& m);
maya::dsl::Element render_tool_call(const ToolCall& tc);
maya::dsl::Element render_streaming_indicator(const Model& m);

// Modals
maya::dsl::Element permission_modal(const Model& m);
maya::dsl::Element model_picker(const Model& m);
maya::dsl::Element thread_list(const Model& m);
maya::dsl::Element command_palette(const Model& m);
maya::dsl::Element diff_review(const Model& m);

} // namespace moha::views
