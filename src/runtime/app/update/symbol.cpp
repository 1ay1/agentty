// symbol_update — reducer for `msg::SymbolMsg`. Parallel to
// mention.cpp (the @file picker); the only differences are the
// candidate type (SymbolEntry vs string) and the chip kind appended on
// select (Attachment::Symbol vs FileRef).
//
// Every arm is now a single call into the shared FilteredPicker: the memo,
// the cold-open refill and the cursor clamp are the primitive's job, not
// something each arm re-derives (and, in the refill's case, something only
// ONE arm used to do — which is why a cold-opened picker that was arrowed
// rather than typed into stayed on "indexing…" forever).

#include "agentty/runtime/app/update/internal.hpp"
#include "agentty/runtime/app/update.hpp"

#include <utility>

#include <maya/core/overload.hpp>

#include "agentty/runtime/composer_attachment.hpp"
#include "agentty/runtime/panel/symbol.hpp"

namespace pn = agentty::ui::panel;

namespace agentty::app::detail {

using maya::overload;

Cmd symbol_update(Model& m, msg::SymbolMsg sm) {
    return std::visit(overload{
        // (No OpenSymbol arm: the panel opens from the COMPOSER's `#`
        // handler, which builds pn::Symbol directly — see composer.cpp.)
        [&](CloseSymbol) -> Cmd {
            ascend(m);   // usually → thread (opened by typing #), or a parent
            return Cmd::none();
        },
        [&](SymbolInput& e) -> Cmd {
            if (auto* o = m.ui.panel.get<pn::Symbol>()) o->picker.type(e.ch);
            return Cmd::none();
        },
        [&](SymbolBackspace) -> Cmd {
            auto* o = m.ui.panel.get<pn::Symbol>();
            if (!o) return Cmd::none();
            // Backspace on an empty query closes the picker. The picker
            // reports "there was nothing to erase" so the test and the
            // mutation cannot drift apart.
            if (!o->picker.backspace()) m.ui.panel.close<pn::Symbol>();
            return Cmd::none();
        },
        [&](SymbolMove& e) -> Cmd {
            if (auto* o = m.ui.panel.get<pn::Symbol>()) o->picker.move(e.delta);
            return Cmd::none();
        },
        [&](SymbolSelect) -> Cmd {
            auto* o = m.ui.panel.get<pn::Symbol>();
            if (!o) return Cmd::none();
            // `selected()` folds the empty-list and out-of-range checks into
            // the type: a null result is the only failure mode.
            const auto* sym = o->picker.selected();
            if (!sym) {
                m.ui.panel.close<pn::Symbol>();
                return Cmd::none();
            }
            Attachment att;
            att.kind        = Attachment::Kind::Symbol;
            att.name        = sym->name;
            att.path        = sym->path;
            att.line_number = sym->line_number;
            m.ui.panel.close<pn::Symbol>();

            std::size_t idx = m.ui.composer.attachments.size();
            m.ui.composer.attachments.push_back(std::move(att));
            auto placeholder = attachment::make_placeholder(idx);
            m.ui.composer.text.insert(m.ui.composer.cursor, placeholder);
            m.ui.composer.cursor += static_cast<int>(placeholder.size());
            return Cmd::none();
        },
    }, sm);
}

} // namespace agentty::app::detail
