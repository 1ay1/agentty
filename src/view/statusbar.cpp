#include "moha/view/statusbar.hpp"

#include <format>
#include <string_view>

#include "moha/view/helpers.hpp"
#include "moha/view/palette.hpp"

namespace moha::ui {

using namespace maya;
using namespace maya::dsl;

namespace {
std::string_view ready_label(Phase p) {
    switch (p) {
        case Phase::Idle:               return "ready";
        case Phase::Streaming:          return "streaming";
        case Phase::AwaitingPermission: return "awaiting permission";
        case Phase::ExecutingTool:      return "executing tool";
    }
    return "?";
}

inline Element shortcut(const char* keys, const char* desc) {
    using namespace maya::dsl;
    return h(text(keys, fg_dim(muted)),
             text(desc, fg_dim(muted))).build();
}
} // namespace

Element status_bar(const Model& m) {
    int pct = m.stream.context_max > 0
                ? (m.stream.tokens_in + m.stream.tokens_out) * 100 / m.stream.context_max
                : 0;
    return h(
        text(" "),
        text(ready_label(m.stream.phase), fg_of(success)),
        text("   "),
        text(std::format("{}%", pct), fg_of(pct > 80 ? danger : muted)),
        spacer(),
        text("Ctrl+/ model  ",     fg_dim(muted)),
        text("Shift+Tab profile  ",fg_dim(muted)),
        text("Ctrl+J threads  ",   fg_dim(muted)),
        text("Ctrl+K palette  ",   fg_dim(muted)),
        text("Ctrl+R review  ",    fg_dim(muted)),
        text("Ctrl+C quit",        fg_dim(muted))
    ).build();
}

} // namespace moha::ui
