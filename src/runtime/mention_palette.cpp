#include "moha/runtime/mention_palette.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <system_error>

#include "moha/tool/util/fs_helpers.hpp"

namespace moha {

namespace {
namespace fs = std::filesystem;

std::string to_lower(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (unsigned char c : s)
        out.push_back(static_cast<char>(std::tolower(c)));
    return out;
}

} // namespace

std::vector<std::string> list_workspace_files(std::size_t cap) {
    std::vector<std::string> out;
    out.reserve(std::min<std::size_t>(cap, 1024));
    const auto& root = tools::util::workspace_root();
    if (root.empty()) return out;

    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(
             root, fs::directory_options::skip_permission_denied, ec);
         it != fs::recursive_directory_iterator() && out.size() < cap;
         it.increment(ec)) {
        if (ec) { ec.clear(); continue; }
        const auto& entry = *it;
        auto fn = entry.path().filename().string();
        const bool is_dir = entry.is_directory(ec);

        // Same skip rules as list_dir.cpp's recursive walker — keep
        // node_modules / build / .git / __pycache__ / etc. out of the
        // candidate set. Otherwise the picker would be drowned in
        // generated artefacts.
        if (is_dir && tools::util::should_skip_dir(fn)) {
            it.disable_recursion_pending();
            continue;
        }
        if (is_dir && it.depth() > 0 && fn.starts_with(".")) {
            it.disable_recursion_pending();
            continue;
        }
        if (is_dir) continue;
        if (!entry.is_regular_file(ec)) continue;

        // Workspace-relative paths read better in the picker UI and
        // round-trip cleanly through the FileRef chip caption.
        std::error_code rec;
        auto rel = fs::relative(entry.path(), root, rec);
        out.push_back(rec ? entry.path().string() : rel.string());
    }
    std::sort(out.begin(), out.end());
    return out;
}

std::vector<std::size_t>
filter_files(const std::vector<std::string>& files, std::string_view query) {
    std::vector<std::size_t> matches;
    matches.reserve(files.size());
    if (query.empty()) {
        for (std::size_t i = 0; i < files.size(); ++i) matches.push_back(i);
        return matches;
    }
    auto needle = to_lower(query);
    for (std::size_t i = 0; i < files.size(); ++i) {
        auto hay = to_lower(files[i]);
        if (hay.find(needle) != std::string::npos) matches.push_back(i);
    }
    return matches;
}

} // namespace moha
