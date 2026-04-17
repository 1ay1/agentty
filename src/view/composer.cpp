#include "moha/view/composer.hpp"

#include <algorithm>
#include <string>

#include "moha/view/palette.hpp"

namespace moha::ui {

using namespace maya;
using namespace maya::dsl;

Element composer(const Model& m) {
    std::string display = m.composer.text;
    std::string placeholder;
    if (display.empty()) {
        placeholder = (m.stream.phase == Phase::Streaming)
            ? "streaming \u2014 type to queue\u2026"
            : "ask a question, or give an instruction\u2026";
    }

    std::string with_cursor = display;
    int cur = std::min<int>(m.composer.cursor, static_cast<int>(display.size()));
    with_cursor.insert(cur, "\u2588");

    int rows = m.composer.expanded ? 8 : 3;

    auto inner = v(
        !display.empty()
            ? text(with_cursor, fg_of(fg))
            : text(placeholder, fg_italic(muted))
    ) | padding(0, 1) | height(rows);

    auto bdr_color = m.stream.phase == Phase::Streaming ? warn
                   : m.stream.phase == Phase::AwaitingPermission ? danger
                   : muted;

    auto hint = h(
        text("Enter", fg_of(fg)), text(" send  ", fg_dim(muted)),
        text("Alt+Enter", fg_of(fg)), text(" newline  ", fg_dim(muted)),
        text("Ctrl+E", fg_of(fg)), text(" expand", fg_dim(muted))
    );

    return (v(inner.build(), hint.build())
            | border(BorderStyle::Round)
            | bcolor(bdr_color)
           ).build();
}

} // namespace moha::ui
