// tool_body_preview — `web_fetch` tool body.
//
// Json (pretty-printed key/value). Only a DONE web_fetch produces a body;
// otherwise returns false to fall through to the shared Failure fallback.

#include "tool_body_common.hpp"

#include <string>

#include "agentty/runtime/view/palette.hpp"

namespace agentty::ui::detail {

bool web_fetch_body(const ToolUse& tc, maya::ToolBodyPreview::Config& out) {
    using Kind = maya::ToolBodyPreview::Kind;
    if (!tc.is_done()) return false;
    const auto& body = tc.output();
    if (!body.empty()) {
        out.kind = Kind::Json;
        out.text = std::string{body};
        out.text_color = text_tertiary;
    }
    return true;
}

} // namespace agentty::ui::detail
