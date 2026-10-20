#pragma once
// agentty::ThreadDelta — one save of a thread, as the reducer describes it.
//
// A save used to carry the whole Thread by value. On a long thread that
// copy (every tool call's args JSON tree, every output) cost the loop
// thread ~20 ms per tool round. A delta carries only what may have changed
// since the last save the reducer described: built by
// persistence::describe_save, run by persistence::save_thread_delta.

#include <cstddef>
#include <cstdint>
#include <vector>

#include "agentty/domain/conversation.hpp"

namespace agentty {

struct ThreadDelta {
    Thread                     header;   // every field but `messages`
    std::size_t                from = 0; // first persistable message that may differ
    std::vector<Message>       tail;     // persistable messages [from, end)
    std::vector<std::uint64_t> fps;      // fingerprint of every persistable message
};

} // namespace agentty
