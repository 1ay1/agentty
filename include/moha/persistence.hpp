#pragma once

#include <filesystem>
#include <vector>

#include "moha/model.hpp"

namespace moha::persistence {

std::filesystem::path data_dir();
std::filesystem::path threads_dir();

std::vector<Thread> load_all_threads();
void save_thread(const Thread& t);
void delete_thread(const std::string& id);

struct Settings {
    std::string model_id;
    Profile profile = Profile::Write;
    std::vector<std::string> favorite_models;
};
Settings load_settings();
void save_settings(const Settings& s);

std::string new_id();
std::string title_from_first_message(const std::string& text);

} // namespace moha::persistence
