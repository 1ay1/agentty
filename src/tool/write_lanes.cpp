// The dispatcher's per-file write exclusion. See write_lanes.hpp.
#include "agentty/tool/write_lanes.hpp"

#include "agentty/tool/registry.hpp"
#include "agentty/tool/util/fs_helpers.hpp"   // util::workspace_root

#include <algorithm>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <string_view>
#include <system_error>

namespace agentty::tools::write_lanes {

namespace {

namespace fs = std::filesystem;

struct Table { std::vector<std::string> busy; };
maya::guarded<Table>& table() {
    static maya::guarded<Table> t;
    return t;
}

// Argument keys that name the file (or tree) a writing tool acts on. Every
// built-in writer spells its target with one of these; `root`/`dir` cover
// the multi-file writers (replace, rewrite_structural) whose blast radius is
// the tree under them. A writer that names none takes the whole workspace.
constexpr std::string_view kTargetKeys[] = {
    "path", "file_path", "filepath", "filename",
    "source", "destination",          // move
    "root", "dir", "directory",       // replace / rewrite_structural
};

[[nodiscard]] fs::path base_of(const CallContext& ctx) {
    if (!ctx.project.empty())   return fs::path{ctx.project};
    if (!ctx.workspace.empty()) return fs::path{ctx.workspace};
    return util::workspace_root();
}

[[nodiscard]] std::string canonical_lane(const fs::path& base, std::string_view raw) {
    fs::path p{std::string{raw}};
    if (p.is_relative() && !base.empty()) p = base / p;
    std::error_code ec;
    auto c = fs::weakly_canonical(p, ec);
    std::string s = (ec || c.empty() ? p.lexically_normal() : c).generic_string();
    while (s.size() > 1 && s.back() == '/') s.pop_back();
#ifdef _WIN32
    // NTFS compares names case-insensitively; so must the lanes.
    std::ranges::transform(s, s.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
#endif
    return s;
}

}  // namespace

std::vector<std::string> lanes_for(std::string_view, const nlohmann::json& args,
                                   const CallContext& ctx) {
    std::vector<std::string> out;
    bool unsure = !args.is_object();
    if (!unsure) {
        const fs::path base = base_of(ctx);
        for (auto key : kTargetKeys) {
            auto it = args.find(std::string{key});
            if (it == args.end()) continue;
            if (!it->is_string() || it->get_ref<const std::string&>().empty()) {
                unsure = true;
                continue;
            }
            try {
                out.push_back(canonical_lane(base, it->get_ref<const std::string&>()));
            } catch (...) {
                unsure = true;
            }
        }
    }
    // Writes, but we can't tell where: it may write anywhere, so it gets the
    // whole workspace. Safe, never fast-and-wrong.
    if (unsure || out.empty()) return {kWholeWorkspace};
    std::ranges::sort(out);
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

bool overlap(std::string_view a, std::string_view b) noexcept {
    if (a == kWholeWorkspace || b == kWholeWorkspace) return true;
    if (a == b) return true;
    auto under = [](std::string_view s, std::string_view l) {
        return l.size() > s.size() && l.substr(0, s.size()) == s
            && (s.back() == '/' || l[s.size()] == '/');
    };
    return a.size() < b.size() ? under(a, b) : under(b, a);
}

Hold::Hold(const maya::unlocked& cap, std::vector<std::string> lanes, const CallContext& ctx)
    : cap_(cap), lanes_(std::move(lanes)) {
    if (lanes_.empty()) return;
    // Short slices so a cancel reaches a queued call: the holder is another
    // tool call, normally milliseconds of disk work, so real waits are short.
    for (;;) {
        if (ctx.cancelled()) return;   // held() stays false
        auto [claimed, waited_out] = table().wait_with_for(cap_,
            std::chrono::milliseconds(100),
            [](const Table& t, const std::vector<std::string>& want) {
                for (const auto& w : want)
                    for (const auto& b : t.busy)
                        if (overlap(w, b)) return false;
                return true;
            },
            [](Table& t, const std::vector<std::string>& want) {
                // Re-check under the lock: the body also runs when the slice
                // timed out with the predicate still false.
                for (const auto& w : want)
                    for (const auto& b : t.busy)
                        if (overlap(w, b)) return false;
                t.busy.insert(t.busy.end(), want.begin(), want.end());
                return true;
            }, lanes_);
        (void)waited_out;
        if (claimed) { held_ = true; return; }
    }
}

Hold::~Hold() {
    if (lanes_.empty() || !held_) return;
    // with() wakes every waiter on the way out.
    table().with(cap_, [](Table& t, const std::vector<std::string>& mine) {
        for (const auto& m : mine)
            if (auto it = std::ranges::find(t.busy, m); it != t.busy.end())
                t.busy.erase(it);
    }, lanes_);
}

std::size_t held_count(const maya::unlocked& cap) {
    return table().read(cap, [](const Table& t) { return t.busy.size(); });
}

}  // namespace agentty::tools::write_lanes
