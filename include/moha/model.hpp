#pragma once
// moha domain model — strong types, decomposed state, enum reflection.

#include <array>
#include <chrono>
#include <compare>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>
#include <maya/widget/spinner.hpp>

namespace moha {

// ============================================================================
// Strong ID types — compile-time distinct, zero-overhead
// ============================================================================
// Prevents accidental interchange of ThreadId / ToolCallId / ModelId.
// Provides == with string_view for pattern matching against known values.

template <typename Tag>
struct Id {
    std::string value;

    Id() = default;
    explicit Id(std::string s) : value(std::move(s)) {}

    [[nodiscard]] bool empty() const noexcept { return value.empty(); }
    [[nodiscard]] const char* c_str() const noexcept { return value.c_str(); }

    bool operator==(const Id&) const = default;
    auto operator<=>(const Id&) const = default;

    [[nodiscard]] bool operator==(std::string_view sv) const { return value == sv; }

    friend void to_json(nlohmann::json& j, const Id& id) { j = id.value; }
    friend void from_json(const nlohmann::json& j, Id& id) { j.get_to(id.value); }
};

struct ThreadIdTag {};
struct ToolCallIdTag {};
struct ModelIdTag {};
struct CheckpointIdTag {};
struct ToolNameTag {};

using ThreadId     = Id<ThreadIdTag>;
using ToolCallId   = Id<ToolCallIdTag>;
using ModelId      = Id<ModelIdTag>;
using CheckpointId = Id<CheckpointIdTag>;
using ToolName     = Id<ToolNameTag>;

// ============================================================================
// Enums with constexpr reflection
// ============================================================================

enum class Role : uint8_t { User, Assistant, System };
enum class Profile : uint8_t { Write, Ask, Minimal };
enum class Phase : uint8_t { Idle, Streaming, AwaitingPermission, ExecutingTool };

[[nodiscard]] constexpr std::string_view to_string(Role r) noexcept {
    switch (r) {
        case Role::User:      return "user";
        case Role::Assistant: return "assistant";
        case Role::System:    return "system";
    }
    return "?";
}

[[nodiscard]] constexpr std::string_view to_string(Profile p) noexcept {
    switch (p) {
        case Profile::Write:   return "Write";
        case Profile::Ask:     return "Ask";
        case Profile::Minimal: return "Minimal";
    }
    return "?";
}

[[nodiscard]] constexpr std::string_view to_string(Phase p) noexcept {
    switch (p) {
        case Phase::Idle:               return "idle";
        case Phase::Streaming:          return "streaming";
        case Phase::AwaitingPermission: return "permission";
        case Phase::ExecutingTool:      return "working";
    }
    return "?";
}

// ============================================================================
// Domain value objects
// ============================================================================

struct ToolUse {
    enum class Status : uint8_t { Pending, Approved, Running, Done, Error, Rejected };
    ToolCallId     id;
    ToolName       name;
    nlohmann::json args;
    std::string    args_streaming;
    std::string    output;
    Status         status   = Status::Pending;
    bool           expanded = true;
};

struct Message {
    Role        role = Role::User;
    std::string text;
    std::string streaming_text;
    std::vector<ToolUse> tool_calls;
    std::chrono::system_clock::time_point timestamp = std::chrono::system_clock::now();
    std::optional<CheckpointId> checkpoint_id;
};

struct Thread {
    ThreadId    id;
    std::string title;
    std::vector<Message> messages;
    std::chrono::system_clock::time_point created_at = std::chrono::system_clock::now();
    std::chrono::system_clock::time_point updated_at = std::chrono::system_clock::now();
};

struct Hunk {
    enum class Status : uint8_t { Pending, Accepted, Rejected };
    int old_start = 0, old_len = 0, new_start = 0, new_len = 0;
    std::string patch;
    Status status = Status::Pending;
};

struct FileChange {
    std::string path;
    int added   = 0;
    int removed = 0;
    std::vector<Hunk> hunks;
    std::string original_contents;
    std::string new_contents;
};

struct PendingPermission {
    ToolCallId  id;
    ToolName    tool_name;
    std::string reason;
};

struct ModelInfo {
    ModelId     id;
    std::string display_name;
    std::string provider;
    int  context_window = 200000;
    bool favorite       = false;
};

// ============================================================================
// Model sub-states — each owns a single concern
// ============================================================================

struct ComposerState {
    std::string text;
    int  cursor   = 0;
    bool expanded = false;
    std::vector<std::string> queued;
};

struct StreamState {
    Phase phase  = Phase::Idle;
    bool  active = false;
    std::chrono::steady_clock::time_point started{};
    std::chrono::steady_clock::time_point last_tick{};
    int tokens_in  = 0;
    int tokens_out = 0;
    int context_max = 200000;
    std::string status;
    maya::Spinner<maya::SpinnerStyle::Dots> spinner{};
};

struct ModelPickerState {
    bool open  = false;
    int  index = 0;
};

struct ThreadListState {
    bool open  = false;
    int  index = 0;
};

// ── Command palette — enum-driven, no magic indices ──────────────────────

enum class Command : uint8_t {
    NewThread, ReviewChanges, AcceptAll, RejectAll,
    CycleProfile, OpenModels, OpenThreads, OpenPlan, Quit,
};

struct CommandDef {
    Command     id;
    const char* label;
    const char* description;
};

inline constexpr std::array kCommands = std::array{
    CommandDef{Command::NewThread,      "New thread",         "Start a fresh conversation"},
    CommandDef{Command::ReviewChanges,  "Review changes",     "Open diff review pane"},
    CommandDef{Command::AcceptAll,      "Accept all changes", "Apply every pending hunk"},
    CommandDef{Command::RejectAll,      "Reject all changes", "Discard every pending hunk"},
    CommandDef{Command::CycleProfile,   "Cycle profile",      "Write \u2192 Ask \u2192 Minimal"},
    CommandDef{Command::OpenModels,     "Open model picker",  ""},
    CommandDef{Command::OpenThreads,    "Open threads",       ""},
    CommandDef{Command::OpenPlan,      "Open plan",          "View task progress"},
    CommandDef{Command::Quit,          "Quit",               "Exit moha"},
};

struct CommandPaletteState {
    bool open = false;
    std::string query;
    int index = 0;
};

struct DiffReviewState {
    bool open       = false;
    int  file_index = 0;
    int  hunk_index = 0;
};

enum class TodoStatus : uint8_t { Pending, InProgress, Completed };

struct TodoItem {
    std::string content;
    TodoStatus  status = TodoStatus::Pending;
};

struct TodoState {
    bool open = false;
    std::vector<TodoItem> items;
};

// ============================================================================
// Model — application state, decomposed into semantic sub-states
// ============================================================================

struct Model {
    // ── Domain ───────────────────────────────────────────────────────
    Thread              current;
    std::vector<Thread> threads;
    Profile             profile = Profile::Write;

    std::vector<ModelInfo> available_models;
    ModelId                model_id{std::string{"claude-opus-4-5"}};

    std::vector<FileChange>          pending_changes;
    std::optional<PendingPermission> pending_permission;

    // ── UI sub-states ────────────────────────────────────────────────
    ComposerState       composer;
    StreamState         stream;
    ModelPickerState    model_picker;
    ThreadListState     thread_list;
    CommandPaletteState command_palette;
    DiffReviewState     diff_review;
    TodoState           todo;
    int                 thread_scroll = 0;
};

} // namespace moha
