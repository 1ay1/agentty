#include "moha/tool/util/arg_reader.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <span>

namespace moha::tools::util {

using nlohmann::json;

namespace {
// Accept the common aliases the model emits when its tool-use schema gets
// cross-trained with Anthropic's built-in text_editor (`file_path`), or
// when it abbreviates. Lookup is case-sensitive for speed; aliases are the
// escape hatch. Keep this table tiny — every miss pays the walk cost.
constexpr std::string_view kAliasesPath[]       = {"file_path", "filepath", "filename"};
constexpr std::string_view kAliasesFilePath[]   = {"path",      "filepath", "filename"};
constexpr std::string_view kAliasesOldString[]  = {"old_str",   "oldStr"};
constexpr std::string_view kAliasesNewString[]  = {"new_str",   "newStr"};
// Anthropic's built-in text_editor tool uses `file_text` for create-command
// content. Models heavily cross-trained on that spec emit file_text even when
// our schema says "content" — without this alias the file lands empty.
constexpr std::string_view kAliasesContent[]    = {"file_text", "text",
                                                    "file_content", "contents",
                                                    "body", "data"};
// Zed's read_file_tool uses `start_line` / `end_line` rather than
// offset / limit. We accept either so a model trained on Zed's surface
// doesn't page-fault on us.
constexpr std::string_view kAliasesOffset[]     = {"start_line", "start", "from_line"};
constexpr std::string_view kAliasesLimit[]      = {"end_line",   "num_lines", "max_lines",
                                                    "count",      "line_count"};
// Terminal/shell — `cd` / `cwd` / `workdir` / `working_directory` all show up.
constexpr std::string_view kAliasesCd[]         = {"cwd", "workdir", "working_directory",
                                                    "directory"};
constexpr std::string_view kAliasesCommand[]    = {"cmd", "shell_command"};

std::span<const std::string_view> aliases_for(std::string_view key) noexcept {
    if (key == "path")       return {kAliasesPath};
    if (key == "file_path")  return {kAliasesFilePath};
    if (key == "old_string") return {kAliasesOldString};
    if (key == "new_string") return {kAliasesNewString};
    if (key == "content")    return {kAliasesContent};
    if (key == "offset")     return {kAliasesOffset};
    if (key == "limit")      return {kAliasesLimit};
    if (key == "cd")         return {kAliasesCd};
    if (key == "command")    return {kAliasesCommand};
    return {};
}
} // namespace

const json* ArgReader::raw(std::string_view key) const noexcept {
    if (!args_.is_object()) return nullptr;
    if (auto it = args_.find(std::string{key}); it != args_.end())
        return &*it;
    for (auto alt : aliases_for(key)) {
        if (auto it = args_.find(std::string{alt}); it != args_.end())
            return &*it;
    }
    return nullptr;
}

std::string ArgReader::str(std::string_view key,
                           std::string def,
                           std::string* note) const {
    const json* v = raw(key);
    if (!v) return def;
    if (v->is_string()) return v->get<std::string>();
    if (v->is_null()) return def;
    if (v->is_array()) {
        std::string out;
        for (std::size_t i = 0; i < v->size(); ++i) {
            if (i) out += '\n';
            const auto& el = (*v)[i];
            if (el.is_string()) out += el.get<std::string>();
            else                out += el.dump();
        }
        if (note) *note = " (" + std::string{key} + " was an array — joined with newlines)";
        return out;
    }
    // number / bool / object — surface a literal repr so the model can see
    // what it sent and correct on retry.
    std::string out = v->dump();
    if (note) *note = " (" + std::string{key} + " was not a string — coerced)";
    return out;
}

std::optional<std::string> ArgReader::require_str(std::string_view key) const {
    const json* v = raw(key);
    if (!v || v->is_null()) return std::nullopt;
    if (v->is_string()) {
        auto s = v->get<std::string>();
        return s.empty() ? std::nullopt : std::optional<std::string>{std::move(s)};
    }
    // Numbers/bools coerced via dump() — still surface as "has a value".
    auto s = v->dump();
    return s.empty() ? std::nullopt : std::optional<std::string>{std::move(s)};
}

int ArgReader::integer(std::string_view key, int def) const {
    const json* v = raw(key);
    if (!v || v->is_null()) return def;
    if (v->is_number_integer()) return v->get<int>();
    if (v->is_number_float())   return static_cast<int>(v->get<double>());
    if (v->is_string()) {
        const auto& s = v->get_ref<const std::string&>();
        int out = def;
        auto r = std::from_chars(s.data(), s.data() + s.size(), out);
        if (r.ec == std::errc{}) return out;
    }
    return def;
}

bool ArgReader::boolean(std::string_view key, bool def) const {
    const json* v = raw(key);
    if (!v || v->is_null()) return def;
    if (v->is_boolean()) return v->get<bool>();
    if (v->is_number_integer()) return v->get<int>() != 0;
    if (v->is_string()) {
        std::string s = v->get<std::string>();
        std::transform(s.begin(), s.end(), s.begin(),
                       [](unsigned char c){ return static_cast<char>(std::tolower(c)); });
        if (s == "true" || s == "1" || s == "yes")  return true;
        if (s == "false"|| s == "0" || s == "no")   return false;
    }
    return def;
}

} // namespace moha::tools::util
