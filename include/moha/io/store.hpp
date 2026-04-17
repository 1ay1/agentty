#pragma once
// moha::io::Store — concept for thread/settings persistence.

#include <concepts>
#include <string>
#include <string_view>
#include <vector>

#include "moha/model.hpp"
#include "moha/io/persistence.hpp"

namespace moha::io {

template <class S>
concept Store = requires(S& s,
                         const Thread& t,
                         const persistence::Settings& settings) {
    { s.load_threads() }     -> std::same_as<std::vector<Thread>>;
    { s.save_thread(t) }     -> std::same_as<void>;
    { s.load_settings() }    -> std::same_as<persistence::Settings>;
    { s.save_settings(settings) } -> std::same_as<void>;
    { s.new_id() }           -> std::convertible_to<ThreadId>;
    { s.title_from(std::string_view{}) } -> std::convertible_to<std::string>;
};

class FsStore {
public:
    [[nodiscard]] std::vector<Thread> load_threads() {
        return persistence::load_all_threads();
    }
    void save_thread(const Thread& t) {
        persistence::save_thread(t);
    }
    [[nodiscard]] persistence::Settings load_settings() {
        return persistence::load_settings();
    }
    void save_settings(const persistence::Settings& s) {
        persistence::save_settings(s);
    }
    [[nodiscard]] ThreadId new_id() {
        return persistence::new_id();
    }
    [[nodiscard]] std::string title_from(std::string_view text) {
        return persistence::title_from_first_message(text);
    }
};

static_assert(Store<FsStore>);

} // namespace moha::io
