#include "moha/runtime/view/thread.hpp"

#include "moha/runtime/view/conversation.hpp"
#include "moha/runtime/view/welcome_screen.hpp"

namespace moha::ui {

maya::Thread::Config thread_config(const Model& m) {
    maya::Thread::Config cfg;
    if (m.d.current.messages.empty()) {
        cfg.is_empty = true;
        cfg.welcome  = welcome_screen_config(m);
        return cfg;
    }
    cfg.conversation = conversation_config(m);
    return cfg;
}

} // namespace moha::ui
