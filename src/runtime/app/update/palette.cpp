// palette_update + todo_update — reducers for the command palette and
// the todo modal. Both are simple list-modals; the palette is the
// dispatcher for action commands (NewThread, ReviewChanges, etc.), so it
// re-enters the top-level update() to fan a Command::* into the matching
// domain Msg.

#include "moha/runtime/app/update/internal.hpp"
#include "moha/runtime/app/update.hpp"

#include <algorithm>
#include <utility>

#include <maya/core/overload.hpp>

#include "moha/runtime/picker.hpp"
#include "moha/runtime/view/helpers.hpp"

namespace moha::app::detail {

namespace pick = moha::ui::pick;
using maya::overload;

Step palette_update(Model m, msg::CommandPaletteMsg pm) {
    return std::visit(overload{
        [&](OpenCommandPalette) -> Step {
            m.ui.command_palette = palette::Open{};
            return done(std::move(m));
        },
        [&](CloseCommandPalette) -> Step {
            m.ui.command_palette = palette::Closed{};
            return done(std::move(m));
        },
        [&](CommandPaletteInput& e) -> Step {
            auto* o = opened(m.ui.command_palette);
            if (o && static_cast<uint32_t>(e.ch) < 0x80) {
                o->query.push_back(static_cast<char>(e.ch));
                // Reset cursor to the top of the (newly filtered) list so
                // the previous index doesn't point at a now-hidden row.
                o->index = 0;
            }
            return done(std::move(m));
        },
        [&](CommandPaletteBackspace) -> Step {
            auto* o = opened(m.ui.command_palette);
            if (o && !o->query.empty()) {
                o->query.pop_back();
                o->index = 0;
            }
            return done(std::move(m));
        },
        [&](CommandPaletteMove& e) -> Step {
            auto* o = opened(m.ui.command_palette);
            if (!o) return done(std::move(m));
            // Clamp against the *visible* row count, not kCommands.size().
            // Without the upper bound the cursor used to walk off-screen
            // and Enter would silently fall through to the no-match path.
            int sz = static_cast<int>(filtered_commands(o->query).size());
            if (sz <= 0) { o->index = 0; return done(std::move(m)); }
            o->index = std::clamp(o->index + e.delta, 0, sz - 1);
            return done(std::move(m));
        },
        [&](CommandPaletteSelect) -> Step {
            auto* o = opened(m.ui.command_palette);
            if (!o) return done(std::move(m));
            // Resolve cursor → typed Command via the SAME filtered list
            // the view rendered. The previous design switched on the raw
            // o->index against the unfiltered enum, which silently fired
            // the wrong command whenever any query was active.
            auto matches = filtered_commands(o->query);
            m.ui.command_palette = palette::Closed{};
            if (matches.empty()
                || o->index < 0
                || o->index >= static_cast<int>(matches.size()))
                return done(std::move(m));
            switch (matches[static_cast<std::size_t>(o->index)]->id) {
                case Command::NewThread:     return moha::app::update(std::move(m), Msg{NewThread{}});
                case Command::ReviewChanges: return moha::app::update(std::move(m), Msg{OpenDiffReview{}});
                case Command::AcceptAll:     return moha::app::update(std::move(m), Msg{AcceptAllChanges{}});
                case Command::RejectAll:     return moha::app::update(std::move(m), Msg{RejectAllChanges{}});
                case Command::CycleProfile:  return moha::app::update(std::move(m), Msg{CycleProfile{}});
                case Command::OpenModels:    return moha::app::update(std::move(m), Msg{OpenModelPicker{}});
                case Command::OpenThreads:   return moha::app::update(std::move(m), Msg{OpenThreadList{}});
                case Command::OpenPlan:      return moha::app::update(std::move(m), Msg{OpenTodoModal{}});
                case Command::CompactContext:return moha::app::update(std::move(m), Msg{CompactContext{}});
                case Command::Quit:          return moha::app::update(std::move(m), Msg{Quit{}});
            }
            return done(std::move(m));
        },
    }, pm);
}

Step todo_update(Model m, msg::TodoMsg tm) {
    return std::visit(overload{
        [&](OpenTodoModal) -> Step {
            m.ui.todo.open = pick::OpenModal{};
            return done(std::move(m));
        },
        [&](CloseTodoModal) -> Step {
            m.ui.todo.open = pick::Closed{};
            return done(std::move(m));
        },
        [&](UpdateTodos& e) -> Step {
            m.ui.todo.items = std::move(e.items);
            return done(std::move(m));
        },
    }, tm);
}

} // namespace moha::app::detail
