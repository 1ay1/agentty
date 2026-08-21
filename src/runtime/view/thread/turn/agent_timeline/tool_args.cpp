// Shared pure helpers for reading tool args and tool output. See
// tool_args.hpp for intent. No maya / widget dependencies — pure data
// shaping — so both thread.cpp (timeline compact-body) and tool_card.cpp
// (widget render) can link against it without dragging widget headers.

#include "agentty/runtime/view/thread/turn/agent_timeline/tool_args.hpp"

#include <chrono>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>

namespace agentty::ui {

std::string safe_arg(const nlohmann::json& args, const char* key) {
    if (!args.is_object()) return {};
    auto it = args.find(key);
    return it != args.end() && it->is_string()
        ? it->get<std::string>() : std::string{};
}

std::string pick_arg(const nlohmann::json& args,
                     std::initializer_list<const char*> keys) {
    if (!args.is_object()) return {};
    for (const char* k : keys) {
        if (auto it = args.find(k); it != args.end() && it->is_string()) {
            const auto& s = it->get_ref<const std::string&>();
            if (!s.empty()) return s;
        }
    }
    return {};
}

std::string pick_streaming_string(std::string_view raw, std::string_view key) {
    if (raw.empty() || key.empty()) return {};
    // Locate `"key"` as a KEY token: the quote-wrapped key followed (after
    // optional whitespace) by a ':'. Retry on false hits (the same text
    // appearing as a VALUE elsewhere).
    std::string needle;
    needle.reserve(key.size() + 2);
    needle.push_back('"');
    needle.append(key);
    needle.push_back('"');
    std::size_t search = 0;
    while (true) {
        std::size_t k = raw.find(needle, search);
        if (k == std::string_view::npos) return {};
        std::size_t i = k + needle.size();
        while (i < raw.size() && (raw[i] == ' ' || raw[i] == '\t'
                                  || raw[i] == '\n' || raw[i] == '\r')) ++i;
        if (i >= raw.size() || raw[i] != ':') { search = k + 1; continue; }
        ++i;  // past ':'
        while (i < raw.size() && (raw[i] == ' ' || raw[i] == '\t'
                                  || raw[i] == '\n' || raw[i] == '\r')) ++i;
        if (i >= raw.size() || raw[i] != '"') return {};  // value not (yet) a string
        ++i;  // past opening quote
        // Collect until the unescaped closing quote (or buffer end for a
        // truncated stream). Decode the escapes we can see mid-stream.
        std::string out;
        while (i < raw.size()) {
            char c = raw[i];
            if (c == '\\') {
                if (i + 1 >= raw.size()) break;   // dangling escape at EOF
                char e = raw[i + 1];
                switch (e) {
                    case 'n': out.push_back('\n'); break;
                    case 't': out.push_back('\t'); break;
                    case 'r': break;              // drop CR
                    case '"': out.push_back('"'); break;
                    case '\\': out.push_back('\\'); break;
                    case '/': out.push_back('/'); break;
                    default:  out.push_back(e); break;  // \uXXXX etc: keep raw
                }
                i += 2;
                continue;
            }
            if (c == '"') return out;             // closing quote → done
            out.push_back(c);
            ++i;
        }
        return out;  // truncated mid-value: return what streamed so far
    }
}

std::string tool_path_arg(const ToolUse& tc) {
    // Parsed args first — authoritative once the throttled reparse catches up.
    if (auto p = pick_arg(tc.args,
            {"path", "file_path", "filepath", "filename"}); !p.empty())
        return p;
    // Streaming fallback: scrape the raw partial JSON so the filename shows
    // the instant its bytes arrive, not one throttle tick later.
    const std::string_view raw{tc.args_streaming};
    for (std::string_view key : {"path", "file_path", "filepath", "filename"})
        if (auto p = pick_streaming_string(raw, key); !p.empty())
            return p;
    return {};
}

int safe_int_arg(const nlohmann::json& args, const char* key, int def) {
    if (!args.is_object()) return def;
    auto it = args.find(key);
    if (it == args.end()) return def;
    if (it->is_number_unsigned()) {
        const auto value = it->get<std::uint64_t>();
        return value <= static_cast<std::uint64_t>(std::numeric_limits<int>::max())
            ? static_cast<int>(value) : def;
    }
    if (!it->is_number_integer()) return def;
    const auto value = it->get<std::int64_t>();
    if (value < std::numeric_limits<int>::min()
        || value > std::numeric_limits<int>::max())
        return def;
    return static_cast<int>(value);
}

int count_lines(const std::string& s) {
    int n = 0;
    for (char c : s) if (c == '\n') n++;
    return n + (!s.empty() && s.back() != '\n' ? 1 : 0);
}

float tool_elapsed(const ToolUse& tc) {
    auto zero = std::chrono::steady_clock::time_point{};
    auto started = tc.started_at();
    if (started == zero) return 0.0f;
    auto finished = tc.finished_at();
    auto end = finished == zero ? std::chrono::steady_clock::now() : finished;
    auto dt = end - started;
    return std::chrono::duration<float>(dt).count();
}

std::string strip_bash_output_fence(const std::string& s) {
    std::string_view sv{s};
    auto drop_trailer = [&](std::string_view marker) {
        auto pos = sv.rfind(marker);
        if (pos != std::string_view::npos) sv = sv.substr(0, pos);
        while (!sv.empty() && (sv.back() == '\n' || sv.back() == '\r'
                               || sv.back() == ' '  || sv.back() == '\t'))
            sv.remove_suffix(1);
    };
    drop_trailer("\n\n[elapsed:");
    drop_trailer("\n\n[output truncated");

    auto fence = sv.find("```");
    if (fence == std::string_view::npos) return std::string{sv};
    // Allow a leading "Command …\n\n" header before the fence — the failure
    // and timeout branches put one there.
    auto body_start = fence + 3;
    // Skip a language tag (we don't emit one, but be forgiving) and the
    // newline after the opening fence.
    while (body_start < sv.size() && sv[body_start] != '\n') ++body_start;
    if (body_start < sv.size() && sv[body_start] == '\n') ++body_start;

    auto close = sv.rfind("```");
    if (close == std::string_view::npos || close <= body_start)
        return std::string{sv.substr(body_start)};

    auto body_end = close;
    while (body_end > body_start
           && (sv[body_end - 1] == '\n' || sv[body_end - 1] == '\r'))
        --body_end;

    std::string header{sv.substr(0, fence)};
    while (!header.empty() && (header.back() == '\n' || header.back() == '\r'
                               || header.back() == ' '))
        header.pop_back();
    std::string body{sv.substr(body_start, body_end - body_start)};
    if (header.empty()) return body;
    if (body.empty()) return header;
    return header + "\n\n" + body;
}

int parse_exit_code(const std::string& output) {
    struct Marker { const char* text; size_t skip; };
    static constexpr Marker markers[] = {
        {"failed with exit code ", 22},
        {"[exit code ",            11},
    };
    for (const auto& m : markers) {
        auto pos = output.rfind(m.text);
        if (pos == std::string::npos) continue;
        try { return std::stoi(output.substr(pos + m.skip)); }
        catch (...) { return 1; }
    }
    if (output.find("timed out") != std::string::npos) return 124;
    return 0;
}

} // namespace agentty::ui
