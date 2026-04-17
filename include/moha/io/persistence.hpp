#pragma once

#include <filesystem>
#include <vector>

#include "moha/model.hpp"

namespace moha::persistence {

[[nodiscard]] std::filesystem::path data_dir();
[[nodiscard]] std::filesystem::path threads_dir();

[[nodiscard]] std::vector<Thread> load_all_threads();
void save_thread(const Thread& t);
void delete_thread(const ThreadId& id);

struct Settings {
    ModelId model_id;
    Profile profile = Profile::Write;
    std::vector<ModelId> favorite_models;
};
[[nodiscard]] Settings load_settings();
void save_settings(const Settings& s);

[[nodiscard]] ThreadId new_id();
[[nodiscard]] std::string title_from_first_message(std::string_view text);

} // namespace moha::persistence
