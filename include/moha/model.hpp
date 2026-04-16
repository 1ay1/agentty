#pragma once

#include <chrono>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>
#include <maya/widget/spinner.hpp>

namespace moha {

enum class Role { User, Assistant, System };
enum class Profile { Write, Ask, Minimal };
enum class Phase { Idle, Streaming, AwaitingPermission, ExecutingTool };

struct ToolUse {
    enum class Status { Pending, Approved, Running, Done, Error, Rejected };
    std::string id;
    std::string name;
    nlohmann::json args;
    std::string args_streaming;
    std::string output;
    Status status = Status::Pending;
    bool expanded = true;
};

struct Message {
    Role role = Role::User;
    std::string text;
    std::string streaming_text;
    std::vector<ToolUse> tool_calls;
    std::chrono::system_clock::time_point timestamp = std::chrono::system_clock::now();
    std::optional<std::string> checkpoint_id;
};

struct Thread {
    std::string id;
    std::string title;
    std::vector<Message> messages;
    std::chrono::system_clock::time_point created_at = std::chrono::system_clock::now();
    std::chrono::system_clock::time_point updated_at = std::chrono::system_clock::now();
};

struct Hunk {
    enum class Status { Pending, Accepted, Rejected };
    int old_start = 0, old_len = 0, new_start = 0, new_len = 0;
    std::string patch;
    Status status = Status::Pending;
};

struct FileChange {
    std::string path;
    int added = 0;
    int removed = 0;
    std::vector<Hunk> hunks;
    std::string original_contents;
    std::string new_contents;
};

struct PendingPermission {
    std::string tool_call_id;
    std::string tool_name;
    std::string reason;
};

struct ModelInfo {
    std::string id;
    std::string display_name;
    std::string provider;
    int context_window = 200000;
    bool favorite = false;
};

struct Model {
    Thread current;
    std::vector<Thread> threads;
    Phase phase = Phase::Idle;
    Profile profile = Profile::Write;

    std::vector<ModelInfo> available_models;
    std::string model_id = "claude-opus-4-5";

    std::string composer_text;
    int composer_cursor = 0;
    bool composer_expanded = false;
    std::vector<std::string> queued_messages;

    std::vector<FileChange> pending_changes;
    bool show_diff_review = false;
    int diff_review_file_index = 0;
    int diff_review_hunk_index = 0;

    bool show_model_picker = false;
    int model_picker_index = 0;
    bool show_thread_list = false;
    int thread_list_index = 0;
    bool show_command_palette = false;
    std::string command_palette_query;
    int command_palette_index = 0;

    std::optional<PendingPermission> pending_permission;

    int thread_scroll = 0;
    int tokens_in = 0;
    int tokens_out = 0;
    int context_max = 200000;
    std::string status_text;

    std::chrono::steady_clock::time_point stream_started{};
    std::chrono::steady_clock::time_point last_tick{};
    bool stream_active = false;
    maya::Spinner<maya::SpinnerStyle::Dots> spinner{};
};

} // namespace moha
