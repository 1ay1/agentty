#include "moha/tool/registry.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <regex>
#include <sstream>
#include <string>

#include <nlohmann/json.hpp>

#include "moha/io/diff.hpp"

namespace moha::tools {

using json = nlohmann::json;
namespace fs = std::filesystem;

namespace {

std::string read_file(const fs::path& p) {
    std::ifstream ifs(p, std::ios::binary);
    if (!ifs) return {};
    std::ostringstream oss; oss << ifs.rdbuf();
    return oss.str();
}

void write_file(const fs::path& p, const std::string& content) {
    fs::create_directories(p.parent_path());
    std::ofstream ofs(p, std::ios::binary);
    ofs.write(content.data(), (std::streamsize)content.size());
}

// ---- Read ------------------------------------------------------------------
ToolDef tool_read() {
    ToolDef t;
    t.name = ToolName{std::string{"read"}};
    t.description = "Read a file from the filesystem. Returns up to 2000 lines "
                    "starting at an optional offset.";
    t.input_schema = json{
        {"type", "object"},
        {"required", {"path"}},
        {"properties", {
            {"path",   {{"type","string"}, {"description","Absolute or relative path"}}},
            {"offset", {{"type","integer"}, {"description","Start line (1-based)"}}},
            {"limit",  {{"type","integer"}, {"description","Max lines"}}},
        }},
    };
    t.needs_permission = [](Profile p){ return p == Profile::Minimal; };
    t.execute = [](const json& args) -> ExecResult {
        std::string path = args.value("path", "");
        int offset = args.value("offset", 1);
        int limit  = args.value("limit", 2000);
        if (path.empty())
            return std::unexpected(ToolError{"path required"});
        std::error_code ec;
        if (!fs::exists(path, ec))
            return std::unexpected(ToolError{"file not found: " + path});
        auto content = read_file(path);
        std::istringstream iss(content);
        std::ostringstream out;
        std::string line;
        int n = 1;
        int shown = 0;
        while (std::getline(iss, line)) {
            if (n >= offset && shown < limit) {
                out << n << "\t" << line << "\n";
                shown++;
            }
            n++;
        }
        return ToolOutput{out.str(), std::nullopt};
    };
    return t;
}

// ---- Write -----------------------------------------------------------------
ToolDef tool_write() {
    ToolDef t;
    t.name = ToolName{std::string{"write"}};
    t.description = "Write (or overwrite) a file with the given contents. "
                    "Requires permission except in Write profile.";
    t.input_schema = json{
        {"type","object"},
        {"required", {"path","content"}},
        {"properties", {
            {"path",    {{"type","string"}}},
            {"content", {{"type","string"}}},
        }},
    };
    t.needs_permission = [](Profile p){ return p != Profile::Write; };
    t.execute = [](const json& args) -> ExecResult {
        std::string path = args.value("path", "");
        std::string content = args.value("content", "");
        if (path.empty())
            return std::unexpected(ToolError{"path required"});
        std::string original;
        std::error_code ec;
        if (fs::exists(path, ec)) original = read_file(path);
        auto change = diff::compute(path, original, content);
        write_file(path, content);
        std::ostringstream msg;
        msg << "wrote " << path << " (" << change.added << "+ "
            << change.removed << "-)";
        return ToolOutput{msg.str(), std::move(change)};
    };
    return t;
}

// ---- Edit ------------------------------------------------------------------
ToolDef tool_edit() {
    ToolDef t;
    t.name = ToolName{std::string{"edit"}};
    t.description = "Edit a file by replacing an exact old_string with new_string. "
                    "The old_string must be uniquely present.";
    t.input_schema = json{
        {"type","object"},
        {"required", {"path","old_string","new_string"}},
        {"properties", {
            {"path",       {{"type","string"}}},
            {"old_string", {{"type","string"}}},
            {"new_string", {{"type","string"}}},
            {"replace_all",{{"type","boolean"}, {"default", false}}},
        }},
    };
    t.needs_permission = [](Profile p){ return p != Profile::Write; };
    t.execute = [](const json& args) -> ExecResult {
        std::string path = args.value("path", "");
        std::string old_s = args.value("old_string", "");
        std::string new_s = args.value("new_string", "");
        bool all = args.value("replace_all", false);
        if (path.empty())
            return std::unexpected(ToolError{"path required"});
        std::error_code ec;
        if (!fs::exists(path, ec))
            return std::unexpected(ToolError{"file not found: " + path});
        std::string original = read_file(path);
        std::string updated = original;
        if (old_s.empty())
            return std::unexpected(ToolError{"old_string empty"});
        if (all) {
            size_t pos = 0; int n = 0;
            while ((pos = updated.find(old_s, pos)) != std::string::npos) {
                updated.replace(pos, old_s.size(), new_s);
                pos += new_s.size();
                n++;
            }
            if (n == 0) return std::unexpected(ToolError{"old_string not found"});
        } else {
            auto pos = updated.find(old_s);
            if (pos == std::string::npos)
                return std::unexpected(ToolError{"old_string not found"});
            if (updated.find(old_s, pos + 1) != std::string::npos)
                return std::unexpected(ToolError{"old_string is not unique; pass replace_all=true"});
            updated.replace(pos, old_s.size(), new_s);
        }
        auto change = diff::compute(path, original, updated);
        write_file(path, updated);
        std::ostringstream msg;
        msg << "edited " << path << " (" << change.added << "+ "
            << change.removed << "-)";
        return ToolOutput{msg.str(), std::move(change)};
    };
    return t;
}

// ---- Bash ------------------------------------------------------------------
ToolDef tool_bash() {
    ToolDef t;
    t.name = ToolName{std::string{"bash"}};
    t.description = "Run a shell command. Output is truncated at 30k chars. "
                    "Requires permission outside of Write profile.";
    t.input_schema = json{
        {"type","object"},
        {"required", {"command"}},
        {"properties", {
            {"command", {{"type","string"}}},
            {"timeout", {{"type","integer"}, {"description","Seconds, default 120"}}},
        }},
    };
    t.needs_permission = [](Profile p){ return p != Profile::Write; };
    t.execute = [](const json& args) -> ExecResult {
        std::string cmd = args.value("command", "");
        if (cmd.empty())
            return std::unexpected(ToolError{"command required"});
#ifdef _WIN32
        std::string wrapped = "bash -lc \"" + cmd + "\" 2>&1";
        FILE* pipe = _popen(wrapped.c_str(), "r");
#else
        std::string wrapped = cmd + " 2>&1";
        FILE* pipe = popen(wrapped.c_str(), "r");
#endif
        if (!pipe) return std::unexpected(ToolError{"popen failed"});
        std::ostringstream out;
        std::array<char, 4096> buf{};
        size_t total = 0;
        while (fgets(buf.data(), (int)buf.size(), pipe)) {
            out << buf.data();
            total += std::strlen(buf.data());
            if (total > 30000) { out << "\n[output truncated]"; break; }
        }
#ifdef _WIN32
        int rc = _pclose(pipe);
#else
        int rc = pclose(pipe);
#endif
        std::string output = out.str();
        if (rc != 0) output += "\n[exit code " + std::to_string(rc) + "]";
        return ToolOutput{std::move(output), std::nullopt};
    };
    return t;
}

// ---- Grep ------------------------------------------------------------------
ToolDef tool_grep() {
    ToolDef t;
    t.name = ToolName{std::string{"grep"}};
    t.description = "Search for a regex pattern across files.";
    t.input_schema = json{
        {"type","object"},
        {"required", {"pattern"}},
        {"properties", {
            {"pattern", {{"type","string"}}},
            {"path",    {{"type","string"}, {"description","Directory root (default: cwd)"}}},
            {"glob",    {{"type","string"}, {"description","File glob (e.g. *.cpp)"}}},
        }},
    };
    t.needs_permission = [](Profile){ return false; };
    t.execute = [](const json& args) -> ExecResult {
        std::string pat = args.value("pattern", "");
        std::string root = args.value("path", ".");
        std::string glob = args.value("glob", "");
        if (pat.empty()) return std::unexpected(ToolError{"pattern required"});
        std::regex re;
        try { re = std::regex(pat); } catch (...) {
            return std::unexpected(ToolError{"bad regex"});
        }
        std::ostringstream out;
        int matches = 0;
        std::error_code ec;
        for (auto it = fs::recursive_directory_iterator(root, ec);
             it != fs::recursive_directory_iterator(); it.increment(ec)) {
            if (ec) break;
            if (!it->is_regular_file(ec)) continue;
            auto p = it->path();
            auto fn = p.filename().string();
            if (!glob.empty()) {
                std::string g = glob;
                for (auto& c : g) if (c == '.') c = '\0';
                if (fn.find(glob.substr(glob.find_last_of('.') == std::string::npos ? 0 : glob.find_last_of('.'))) == std::string::npos)
                    continue;
            }
            std::ifstream ifs(p);
            if (!ifs) continue;
            std::string line;
            int n = 1;
            while (std::getline(ifs, line)) {
                if (std::regex_search(line, re)) {
                    out << p.string() << ":" << n << ":" << line << "\n";
                    if (++matches > 500) { out << "[>500 matches, truncated]\n"; goto done; }
                }
                n++;
            }
        }
        done:
        if (matches == 0) return ToolOutput{"no matches", std::nullopt};
        return ToolOutput{out.str(), std::nullopt};
    };
    return t;
}

// ---- Glob ------------------------------------------------------------------
ToolDef tool_glob() {
    ToolDef t;
    t.name = ToolName{std::string{"glob"}};
    t.description = "Find files by name pattern (simple substring match).";
    t.input_schema = json{
        {"type","object"},
        {"required", {"pattern"}},
        {"properties", {
            {"pattern", {{"type","string"}}},
            {"path",    {{"type","string"}}},
        }},
    };
    t.needs_permission = [](Profile){ return false; };
    t.execute = [](const json& args) -> ExecResult {
        std::string pat = args.value("pattern", "");
        std::string root = args.value("path", ".");
        std::ostringstream out;
        int n = 0;
        std::error_code ec;
        for (auto it = fs::recursive_directory_iterator(root, ec);
             it != fs::recursive_directory_iterator(); it.increment(ec)) {
            if (ec) break;
            if (!it->is_regular_file(ec)) continue;
            auto s = it->path().string();
            if (s.find(pat) != std::string::npos) {
                out << s << "\n";
                if (++n > 500) { out << "[>500, truncated]\n"; break; }
            }
        }
        if (n == 0) return ToolOutput{"no matches", std::nullopt};
        return ToolOutput{out.str(), std::nullopt};
    };
    return t;
}

// ---- Todo ------------------------------------------------------------------
ToolDef tool_todo() {
    ToolDef t;
    t.name = ToolName{std::string{"todo"}};
    t.description = "Maintain the session todo list (overwrites full list).";
    t.input_schema = json{
        {"type","object"},
        {"required", {"todos"}},
        {"properties", {
            {"todos", {{"type","array"},
                {"items", {{"type","object"},
                    {"properties", {
                        {"content", {{"type","string"}}},
                        {"status",  {{"type","string"},
                            {"enum", {"pending","in_progress","completed"}}}},
                    }},
                    {"required", {"content","status"}},
                }},
            }},
        }},
    };
    t.needs_permission = [](Profile){ return false; };
    t.execute = [](const json& args) -> ExecResult {
        auto todos = args.value("todos", json::array());
        std::ostringstream out;
        for (const auto& td : todos) {
            std::string st = td.value("status", "pending");
            char mark = st == "completed" ? 'x' : st == "in_progress" ? '-' : ' ';
            out << "[" << mark << "] " << td.value("content", "") << "\n";
        }
        return ToolOutput{out.str(), std::nullopt};
    };
    return t;
}

std::vector<ToolDef> build_registry() {
    std::vector<ToolDef> r;
    r.push_back(tool_read());
    r.push_back(tool_write());
    r.push_back(tool_edit());
    r.push_back(tool_bash());
    r.push_back(tool_grep());
    r.push_back(tool_glob());
    r.push_back(tool_todo());
    return r;
}

} // namespace

const std::vector<ToolDef>& registry() {
    static const std::vector<ToolDef> r = build_registry();
    return r;
}

const ToolDef* find(std::string_view name) {
    for (const auto& t : registry()) if (t.name == name) return &t;
    return nullptr;
}

} // namespace moha::tools
