#pragma once
// agentty ↔ jaal: facts about agentty's types that jaal can't see by itself.
//
// jaal checks every message type is Sendable (safe to move to another
// thread) by looking inside plain structs field by field. A class with a
// constructor or private members is opaque to that check, so it's rejected
// unless it opts in here. Every opt-in is a claim a human made; each one
// says why it's true.
//
// This header is only included where jaal is available (the jaal-based
// runtime, and the check in tests/jaal_sendable_test.cpp).

#include <jaal/core/sendable.hpp>

#include <nlohmann/json.hpp>

#include <agentty/domain/conversation.hpp>
#include <agentty/domain/id.hpp>
#include <agentty/domain/lazy_bytes.hpp>

// Id<Tag> is a strong newtype around ONE std::string (domain/id.hpp). It
// owns its value outright; a moved Id shares nothing with the source. It
// only fails jaal's structural check because it has constructors.
template <class Tag>
inline constexpr bool jaal::sendable_opt_in<agentty::Id<Tag>> = true;

// LazyBytes shares an immutable cell between copies (shared_ptr<const
// Cell>). The one mutation, materialising the bytes, happens exactly once
// under std::call_once, and the resolver pointer is atomic, so any number
// of threads may read copies of the same payload at once. See the
// thread-safety note in domain/lazy_bytes.hpp and the concurrent test in
// tests/lazy_image_test.cpp.
template <>
inline constexpr bool jaal::sendable_opt_in<agentty::LazyBytes> = true;

// ImageContent (domain/conversation.hpp) is a media-type string plus a
// LazyBytes, both behind a class interface. Its only mutation from const
// code is LazyBytes materialising, covered above. It holds nothing borrowed.
template <>
inline constexpr bool jaal::sendable_opt_in<agentty::ImageContent> = true;

// nlohmann::json (tool-call args in domain/conversation.hpp) owns its whole
// tree: objects, arrays and strings are heap nodes the value owns, and a
// moved-from json is left null. Checked under tsan: moving a json to
// another thread and mutating it there shares nothing with the source.
// Sendable is about MOVING; json is not Frozen (and isn't claimed to be),
// so it still can't go in a jaal::shared<T>.
template <>
inline constexpr bool jaal::sendable_opt_in<nlohmann::json> = true;
