#pragma once
// The disk, as mcp-cpp's FileSystem. mcp-cpp does no IO; agentty hands it
// this. Writes are atomic (sibling temp, fsync, rename), keep the target's
// mode, and go through a symlink to its target instead of replacing it.

#include <mcp/tools/files.hpp>

#include <filesystem>
#include <string>
#include <string_view>

namespace agentty::tools::util {

namespace fs = std::filesystem;

class DiskFiles final : public ::mcp::tools::FileSystem {
    using FileStat = ::mcp::tools::FileStat;
    using DirEntry = ::mcp::tools::DirEntry;
    using WalkStep = ::mcp::tools::WalkStep;

public:
    [[nodiscard]] FileStat stat(const fs::path& p) const override;
    [[nodiscard]] fs::path canonical(const fs::path& p, std::error_code& ec) const override;
    [[nodiscard]] fs::path read_symlink(const fs::path& p, std::error_code& ec) const override;
    [[nodiscard]] fs::path cwd() const override;
    [[nodiscard]] fs::path scratch_dir() const override;
    [[nodiscard]] std::optional<std::string> read(const fs::path& p) const override;
    [[nodiscard]] std::optional<std::string> read_prefix(const fs::path& p,
                                                         std::size_t n) const override;
    [[nodiscard]] std::string write_atomic(const fs::path& p, std::string_view content) override;
    [[nodiscard]] std::vector<DirEntry> list(const fs::path& dir,
                                             std::error_code& ec) const override;
    bool walk(const fs::path& root,
              const std::function<WalkStep(const DirEntry&)>& visit) const override;
    bool create_directories(const fs::path& p, std::error_code& ec) override;
    bool remove(const fs::path& p, std::error_code& ec) override;
    std::uintmax_t remove_all(const fs::path& p, std::error_code& ec) override;
    void rename(const fs::path& from, const fs::path& to, std::error_code& ec) override;
    void copy(const fs::path& from, const fs::path& to, std::error_code& ec) override;
};

}  // namespace agentty::tools::util
