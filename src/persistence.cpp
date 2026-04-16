#include "moha/persistence.hpp"

#include <cstdlib>
#include <fstream>
#include <random>
#include <sstream>

#include <nlohmann/json.hpp>

namespace moha::persistence {

namespace fs = std::filesystem;
using json = nlohmann::json;

fs::path data_dir() {
    const char* home = std::getenv("USERPROFILE");
    if (!home) home = std::getenv("HOME");
    fs::path p = home ? fs::path(home) : fs::current_path();
    p /= ".moha";
    std::error_code ec;
    fs::create_directories(p, ec);
    return p;
}

fs::path threads_dir() {
    auto p = data_dir() / "threads";
    std::error_code ec;
    fs::create_directories(p, ec);
    return p;
}

static std::string role_to_string(Role r) {
    switch (r) {
        case Role::User: return "user";
        case Role::Assistant: return "assistant";
        case Role::System: return "system";
    }
    return "user";
}
static Role role_from_string(const std::string& s) {
    if (s == "assistant") return Role::Assistant;
    if (s == "system")    return Role::System;
    return Role::User;
}

static json message_to_json(const Message& m) {
    json j;
    j["role"] = role_to_string(m.role);
    j["text"] = m.text;
    j["timestamp"] = std::chrono::duration_cast<std::chrono::seconds>(
        m.timestamp.time_since_epoch()).count();
    json tcs = json::array();
    for (const auto& tc : m.tool_calls) {
        json t;
        t["id"] = tc.id;
        t["name"] = tc.name;
        t["args"] = tc.args;
        t["output"] = tc.output;
        t["status"] = static_cast<int>(tc.status);
        tcs.push_back(std::move(t));
    }
    j["tool_calls"] = std::move(tcs);
    if (m.checkpoint_id) j["checkpoint_id"] = *m.checkpoint_id;
    return j;
}

static Message message_from_json(const json& j) {
    Message m;
    m.role = role_from_string(j.value("role", "user"));
    m.text = j.value("text", "");
    if (j.contains("timestamp"))
        m.timestamp = std::chrono::system_clock::time_point{
            std::chrono::seconds{j["timestamp"].get<long long>()}};
    if (j.contains("tool_calls")) {
        for (const auto& t : j["tool_calls"]) {
            ToolUse tc;
            tc.id = t.value("id", "");
            tc.name = t.value("name", "");
            tc.args = t.value("args", json::object());
            tc.output = t.value("output", "");
            tc.status = static_cast<ToolUse::Status>(t.value("status", 0));
            m.tool_calls.push_back(std::move(tc));
        }
    }
    if (j.contains("checkpoint_id")) m.checkpoint_id = j["checkpoint_id"].get<std::string>();
    return m;
}

std::vector<Thread> load_all_threads() {
    std::vector<Thread> out;
    std::error_code ec;
    if (!fs::exists(threads_dir(), ec)) return out;
    for (const auto& e : fs::directory_iterator(threads_dir(), ec)) {
        if (e.path().extension() != ".json") continue;
        std::ifstream ifs(e.path());
        if (!ifs) continue;
        try {
            json j; ifs >> j;
            Thread t;
            t.id = j.value("id", "");
            t.title = j.value("title", "");
            if (j.contains("created_at"))
                t.created_at = std::chrono::system_clock::time_point{
                    std::chrono::seconds{j["created_at"].get<long long>()}};
            if (j.contains("updated_at"))
                t.updated_at = std::chrono::system_clock::time_point{
                    std::chrono::seconds{j["updated_at"].get<long long>()}};
            for (const auto& mj : j.value("messages", json::array()))
                t.messages.push_back(message_from_json(mj));
            out.push_back(std::move(t));
        } catch (...) {
            continue;
        }
    }
    std::sort(out.begin(), out.end(), [](const Thread& a, const Thread& b){
        return a.updated_at > b.updated_at;
    });
    return out;
}

void save_thread(const Thread& t) {
    if (t.id.empty() || t.messages.empty()) return;
    json j;
    j["id"] = t.id;
    j["title"] = t.title;
    j["created_at"] = std::chrono::duration_cast<std::chrono::seconds>(
        t.created_at.time_since_epoch()).count();
    j["updated_at"] = std::chrono::duration_cast<std::chrono::seconds>(
        t.updated_at.time_since_epoch()).count();
    json msgs = json::array();
    for (const auto& m : t.messages) msgs.push_back(message_to_json(m));
    j["messages"] = std::move(msgs);
    std::ofstream ofs(threads_dir() / (t.id + ".json"));
    ofs << j.dump(2);
}

void delete_thread(const std::string& id) {
    std::error_code ec;
    fs::remove(threads_dir() / (id + ".json"), ec);
}

Settings load_settings() {
    Settings s;
    std::ifstream ifs(data_dir() / "settings.json");
    if (!ifs) return s;
    try {
        json j; ifs >> j;
        s.model_id = j.value("model_id", "");
        s.profile = static_cast<Profile>(j.value("profile", 0));
        s.favorite_models = j.value("favorite_models", std::vector<std::string>{});
    } catch (...) {}
    return s;
}

void save_settings(const Settings& s) {
    json j;
    j["model_id"] = s.model_id;
    j["profile"] = static_cast<int>(s.profile);
    j["favorite_models"] = s.favorite_models;
    std::ofstream ofs(data_dir() / "settings.json");
    ofs << j.dump(2);
}

std::string new_id() {
    static std::mt19937_64 rng{std::random_device{}()};
    std::uniform_int_distribution<uint64_t> dist;
    std::ostringstream oss;
    oss << std::hex << dist(rng);
    return oss.str();
}

std::string title_from_first_message(const std::string& text) {
    std::string t = text;
    for (auto& c : t) if (c == '\n' || c == '\r') c = ' ';
    if (t.size() > 60) { t.resize(57); t += "..."; }
    if (t.empty()) t = "New thread";
    return t;
}

} // namespace moha::persistence
