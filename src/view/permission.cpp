#include "moha/view/permission.hpp"

#include <string>
#include <vector>

#include "moha/view/palette.hpp"

namespace moha::ui {

using namespace maya;
using namespace maya::dsl;

namespace {
// Derive a human-readable "always" pattern from a tool's args.
// bash → "bash <first word>*"; file tools → directory or "<tool> *"; else → tool name.
std::string derive_always_pattern(const ToolUse& tc) {
    if (tc.name == "bash") {
        std::string cmd = tc.args.value("command", "");
        auto end = cmd.find_first_of(" \t\n");
        std::string first = (end == std::string::npos) ? cmd : cmd.substr(0, end);
        if (first.empty()) return "bash *";
        return "bash " + first + " *";
    }
    if (tc.name == "read" || tc.name == "edit" || tc.name == "write") {
        std::string path = tc.args.value("path", "");
        if (path.empty()) return tc.name.value + " *";
        auto slash = path.find_last_of('/');
        if (slash != std::string::npos)
            return tc.name.value + " " + path.substr(0, slash) + "/*";
        return tc.name.value + " " + path;
    }
    return tc.name.value + " *";
}
} // namespace

Element render_inline_permission(const PendingPermission& pp, const ToolUse& tc) {
    auto pattern    = derive_always_pattern(tc);
    auto allow_key  = fg_bold(success);
    auto deny_key   = fg_bold(danger);
    auto label      = fg_of(muted);
    auto pattern_st = fg_bold(warn);

    auto footer_row = h(
        text("[", label),  text("Y", allow_key), text("] Allow  ", label),
        text("[", label),  text("N", deny_key),  text("] Deny  ",  label),
        spacer(),
        text("[", label),  text("A", allow_key), text("] Always for ", label),
        text(pattern, pattern_st),
        text(" \u25BE", label.with_dim())
    );

    std::vector<Element> rows;
    if (!pp.reason.empty())
        rows.push_back(text(pp.reason, fg_italic(muted)).build());
    rows.push_back(footer_row.build());

    return (v(std::move(rows))
            | border(BorderStyle::Round)
            | bcolor(warn)
            | padding(0, 1, 0, 1)).build();
}

Element render_checkpoint_divider() {
    auto d = fg_dim(muted);
    return h(
        text("\u2500\u2500\u2500 ", d),
        text("[", d),
        text("\u21BA Restore checkpoint", fg_of(warn)),
        text("] ", d),
        text("\u2500\u2500\u2500", d),
        spacer()
    ).build();
}

} // namespace moha::ui
