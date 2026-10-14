// The disk as mcp-cpp's FileSystem, plus the read/write helpers agentty's
// own code uses. The one place agentty's tool layer touches file bytes.
#include "agentty/tool/util/disk_files.hpp"
#include "agentty/tool/util/fs_helpers.hpp"   // read_file / write_file

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#  include <io.h>
#  include <fcntl.h>
#  include <share.h>
#  include <process.h>
#  include <sys/stat.h>
#else
#  include <fcntl.h>
#  include <unistd.h>
#  include <sys/stat.h>
#endif

namespace agentty::tools::util {

using ::mcp::tools::DirEntry;
using ::mcp::tools::FileStat;
using ::mcp::tools::WalkStep;

namespace {
// Translate the most common filesystem errno values to a sentence the
// model can act on. Raw `strerror` reads as "Permission denied" /
// "No such file or directory" — fine for humans, but the LLM responds
// better to the longer form ("you don't have write permission to X")
// when it's deciding whether to retry as a different path or surface a
// human ask. The caller appends the path/operation context.
std::string explain_errno(int e) {
    switch (e) {
        case EACCES:        return "permission denied";
        case EPERM:         return "operation not permitted (privileged op)";
        case ENOENT:        return "path not found";
        case ENOTDIR:       return "expected a directory but found a file";
        case EISDIR:        return "expected a file but found a directory";
        case ENOSPC:        return "out of disk space";
        case EROFS:         return "filesystem is read-only";
        case EMFILE:
        case ENFILE:        return "too many open files (process FD limit hit)";
        case ELOOP:         return "symlink loop";
        case ENAMETOOLONG:  return "path is too long";
        case EBUSY:         return "file is busy / locked by another process";
#ifdef EDQUOT
        case EDQUOT:        return "disk quota exceeded";
#endif
        default:            return std::strerror(e);
    }
}

}  // namespace

std::string read_file(const fs::path& p) {
    // Size-then-read: avoid the ifstream→ostringstream→.str() chain, which
    // double-allocates and copies through the streambuf. One stat, one
    // malloc, one read. Fallback to streambuf drain if the size isn't known
    // (e.g. /proc, pipes) — those produce file_size==0 or throw.
    std::error_code ec;
    auto sz = fs::file_size(p, ec);
#ifdef _WIN32
    // Native Win32 path: CreateFileW with FILE_FLAG_SEQUENTIAL_SCAN tells
    // the cache manager to read ahead aggressively (larger prefetch window,
    // pages discarded after use). For the read / grep / edit hot path —
    // one linear pass, no seeks — this is materially faster than the CRT
    // ReadFile that std::ifstream lowers to. Also avoids the CRT's
    // per-char buffer translation overhead on large files.
    if (!ec && sz > 0) {
        HANDLE h = ::CreateFileW(p.wstring().c_str(),
                                 GENERIC_READ,
                                 FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                 nullptr,
                                 OPEN_EXISTING,
                                 FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN,
                                 nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            std::string out;
            out.resize(sz);
            char*  buf   = out.data();
            size_t total = 0;
            const size_t want = static_cast<size_t>(sz);
            while (total < want) {
                DWORD chunk = static_cast<DWORD>(
                    std::min<size_t>(want - total, 1u << 20));   // 1 MiB reads
                DWORD got = 0;
                if (!::ReadFile(h, buf + total, chunk, &got, nullptr) || got == 0)
                    break;
                total += got;
            }
            ::CloseHandle(h);
            if (total != want) out.resize(total);
            return out;
        }
        // Fall through to ifstream on CreateFileW failure (rare: locked /
        // permission-denied files where the CRT might still retry through
        // a different path). Better to degrade gracefully than refuse.
    }
#endif
    std::ifstream ifs(p, std::ios::binary);
    if (!ifs) return {};
    if (!ec && sz > 0) {
        std::string out;
        out.resize(sz);
        ifs.read(out.data(), static_cast<std::streamsize>(sz));
        if (auto got = ifs.gcount(); static_cast<uintmax_t>(got) != sz)
            out.resize(static_cast<size_t>(got));
        return out;
    }
    std::ostringstream oss; oss << ifs.rdbuf();
    return oss.str();
}

std::string write_file(const fs::path& p, std::string_view content) {
    // Follow a symlink to what it points AT before writing anything.
    //
    // The publish step below is rename(), which replaces the name it is given
    // -- and for a symlink that means replacing the LINK. Without this, an
    // edit through a symlink silently did three wrong things at once: the
    // link became a regular file, the real target kept its old contents, and
    // the tool reported success with a diff of a change that went to a brand
    // new inode nobody was looking at. Dotfiles repos and generated-file
    // symlinks (compile_commands.json) hit this immediately.
    //
    // Confinement is unaffected: is_within_workspace canonicalises before we
    // are ever called, so a link pointing outside the workspace root was
    // already refused. Resolving here can only land inside it.
    fs::path real = p;
    {
        std::error_code ec;
        if (fs::is_symlink(real, ec) && !ec) {
            auto resolved = fs::weakly_canonical(real, ec);
            if (!ec && !resolved.empty()) real = std::move(resolved);
        }
    }

    auto parent = real.parent_path();
    if (!parent.empty()) {
        std::error_code ec;
        fs::create_directories(parent, ec);
        if (ec) return "failed to create directory '" + parent.string() + "': " + ec.message();
    }

    // Atomic write: write to a sibling temp file, fsync, rename over target.
    // Crash or power-loss mid-write leaves the original file intact; only
    // the fully-written, fsync'd copy ever becomes visible at the target
    // path. The temp must live in the same directory so the rename is a
    // single filesystem operation — cross-device rename falls back to a
    // non-atomic copy.
    //
    // The temp name is made unique by creating it exclusively (O_EXCL) and
    // retrying with a new suffix if it exists, so no counter is shared
    // between concurrent writers. The suffix mixes pid, a stack address and
    // the attempt, which is enough to make collisions rare.
#ifdef _WIN32
    const unsigned long pid = static_cast<unsigned long>(::GetCurrentProcessId());
#else
    const unsigned long pid = static_cast<unsigned long>(::getpid());
#endif
    const auto salt = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(&pid));
    fs::path tmp;
    auto tmp_name = [&](int attempt) {
        fs::path t = real;
        t += fs::path(".agentty-tmp-" + std::to_string(pid) + "-"
                      + std::to_string((salt >> 4) ^ (static_cast<std::uint64_t>(attempt) * 0x9e3779b97f4a7c15ULL)));
        return t;
    };

    // Preserve existing mode on POSIX so the rename doesn't regress perms.
#ifndef _WIN32
    mode_t target_mode = 0644;
    bool   had_mode    = false;
    {
        struct stat st{};
        if (::stat(real.c_str(), &st) == 0) {
            target_mode = st.st_mode & 07777;
            had_mode    = true;
        }
    }
#endif

    // Drop down to the POSIX/Win32 fd so we can fsync before the rename.
    // ofstream::flush only empties the libstdc++ streambuf into the OS —
    // power-loss / crash can still lose the bytes, and on some FUSE and
    // network filesystems the data isn't readable by the next open until
    // fsync completes.
#ifdef _WIN32
    // Use the wide-char variant so Unicode paths (e.g. under a non-ASCII
    // %USERPROFILE%) round-trip correctly. `_sopen_s` takes an ANSI/MBCS
    // path which silently corrupts multi-byte sequences on some MinGW
    // ucrt configurations.
    int fd = -1;
    for (int attempt = 0; attempt < 16 && fd < 0; ++attempt) {
        tmp = tmp_name(attempt);
        auto ws = tmp.wstring();
        if (::_wsopen_s(&fd, ws.c_str(),
                        _O_WRONLY | _O_CREAT | _O_EXCL | _O_BINARY,
                        _SH_DENYNO, _S_IREAD | _S_IWRITE) != 0) {
            fd = -1;
            if (errno != EEXIST) break;
        }
    }
    if (fd < 0)
        return "cannot open '" + p.string() + "' for writing";
#else
    int fd = -1;
    for (int attempt = 0; attempt < 16 && fd < 0; ++attempt) {
        tmp = tmp_name(attempt);
        fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
        if (fd < 0 && errno != EEXIST) break;
    }
    if (fd < 0)
        return std::string("cannot open '") + p.string() + "' for writing: "
             + explain_errno(errno);
#endif

    auto cleanup_tmp = [&] {
        std::error_code ec;
        fs::remove(tmp, ec);
    };

    const char* data = content.data();
    size_t remaining = content.size();
    while (remaining > 0) {
#ifdef _WIN32
        int n2 = _write(fd, data, static_cast<unsigned>(
            remaining > 0x7fffffff ? 0x7fffffff : remaining));
#else
        ssize_t n2 = ::write(fd, data, remaining);
        if (n2 < 0 && errno == EINTR) continue;
#endif
        if (n2 <= 0) {
            std::string err = std::string("write to '") + p.string()
                + "' failed: " + explain_errno(errno);
#ifdef _WIN32
            _close(fd);
#else
            ::close(fd);
#endif
            cleanup_tmp();
            return err;
        }
        data += n2;
        remaining -= static_cast<size_t>(n2);
    }

    // fsync temp: data is durable before the rename publishes it.
#ifdef _WIN32
    (void)_commit(fd);
    _close(fd);
#else
    if (had_mode) (void)::fchmod(fd, target_mode);
    // Linux/FreeBSD have fdatasync (skips metadata flush, slightly faster);
    // macOS only has fsync. The durability guarantee we need is the same —
    // file contents committed to disk before the rename publishes them —
    // so falling back to fsync on Apple costs a metadata block at most.
#if defined(__APPLE__)
    (void)::fsync(fd);
#else
    (void)::fdatasync(fd);
#endif
    ::close(fd);
#endif

    // Atomic publish. Windows MoveFileExW with REPLACE_EXISTING is atomic on
    // NTFS (single MFT update); WRITE_THROUGH flushes the directory entry.
    // POSIX rename() is atomic by spec when src/dst are on the same FS.
#ifdef _WIN32
    auto tmp_w = tmp.wstring();
    auto dst_w = real.wstring();
    if (!::MoveFileExW(tmp_w.c_str(), dst_w.c_str(),
                       MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DWORD e = ::GetLastError();
        cleanup_tmp();
        return "atomic rename to '" + p.string() + "' failed (GLE=" + std::to_string(e) + ")";
    }
#else
    if (::rename(tmp.c_str(), real.c_str()) != 0) {
        std::string err = std::string("atomic rename to '") + p.string()
            + "' failed: " + explain_errno(errno);
        cleanup_tmp();
        return err;
    }
    // fsync parent dir so the rename itself survives power loss.
    if (!parent.empty()) {
        int dfd = ::open(parent.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (dfd >= 0) { (void)::fsync(dfd); ::close(dfd); }
    }
#endif
    return {};
}

namespace {

FileStat stat_of(const fs::directory_entry& e, int depth, DirEntry* out) {
    FileStat s;
    std::error_code ec;
    s.symlink   = e.is_symlink(ec);
    s.directory = e.is_directory(ec);
    s.regular   = e.is_regular_file(ec);
    s.exists    = true;
    if (s.regular) { auto z = e.file_size(ec); if (!ec) s.size = z; }
    auto t = e.last_write_time(ec);
    if (!ec) s.mtime = t;
    if (out) {
        out->path = e.path(); out->regular = s.regular; out->directory = s.directory;
        out->symlink = s.symlink; out->size = s.size; out->mtime = s.mtime; out->depth = depth;
    }
    return s;
}

}  // namespace

FileStat DiskFiles::stat(const fs::path& p) const {
    FileStat s;
    std::error_code ec;
    const auto st = fs::status(p, ec);
    if (ec || !fs::exists(st)) {
        // A dangling symlink still "is" something to a tool that lists it.
        const auto lst = fs::symlink_status(p, ec);
        if (!ec && fs::is_symlink(lst)) { s.exists = true; s.symlink = true; }
        return s;
    }
    s.exists    = true;
    s.regular   = fs::is_regular_file(st);
    s.directory = fs::is_directory(st);
    s.symlink   = fs::is_symlink(fs::symlink_status(p, ec));
    if (s.regular) { auto z = fs::file_size(p, ec); if (!ec) s.size = z; }
    auto t = fs::last_write_time(p, ec);
    if (!ec) s.mtime = t;
    return s;
}

fs::path DiskFiles::canonical(const fs::path& p, std::error_code& ec) const {
    return fs::weakly_canonical(p, ec);
}

fs::path DiskFiles::read_symlink(const fs::path& p, std::error_code& ec) const {
    return fs::read_symlink(p, ec);
}

fs::path DiskFiles::cwd() const {
    std::error_code ec;
    auto c = fs::current_path(ec);
    return ec ? fs::path{} : c;
}

fs::path DiskFiles::scratch_dir() const {
    std::error_code ec;
    auto t = fs::temp_directory_path(ec);
    return ec ? fs::path{} : t;
}

std::optional<std::string> DiskFiles::read(const fs::path& p) const {
    std::error_code ec;
    if (!fs::exists(p, ec) || ec) return std::nullopt;
    {
        std::ifstream probe(p, std::ios::binary);
        if (!probe) return std::nullopt;
    }
    return read_file(p);
}

std::optional<std::string> DiskFiles::read_prefix(const fs::path& p, std::size_t n) const {
    std::ifstream ifs(p, std::ios::binary);
    if (!ifs) return std::nullopt;
    std::string out(n, '\0');
    ifs.read(out.data(), static_cast<std::streamsize>(n));
    out.resize(static_cast<std::size_t>(ifs.gcount()));
    return out;
}

std::string DiskFiles::write_atomic(const fs::path& p, std::string_view content) {
    return write_file(p, content);
}

std::vector<DirEntry> DiskFiles::list(const fs::path& dir, std::error_code& ec) const {
    std::vector<DirEntry> out;
    fs::directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec);
    if (ec) return out;
    for (; it != fs::directory_iterator(); it.increment(ec)) {
        if (ec) { ec.clear(); continue; }
        DirEntry d;
        stat_of(*it, 0, &d);
        out.push_back(std::move(d));
    }
    return out;
}

bool DiskFiles::walk(const fs::path& root,
                            const std::function<WalkStep(const DirEntry&)>& visit) const {
    std::error_code ec;
    // A file root is a walk of one. recursive_directory_iterator on a
    // regular file fails with ENOTDIR, which silently made the builtin
    // grep (the only one on a host without ripgrep -- a stock Mac) report
    // "No matches" for `grep path=src/foo.cpp`.
    if (fs::directory_entry self(root, ec); !ec && !self.is_directory(ec)) {
        if (ec || !self.exists(ec)) return false;
        DirEntry d;
        stat_of(self, 0, &d);
        (void)visit(d);
        return true;
    }
    ec.clear();
    fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec);
    if (ec) return false;
    for (; it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) { ec.clear(); continue; }
        DirEntry d;
        stat_of(*it, it.depth(), &d);
        // Never through a symlinked directory: a cyclic link never ends.
        if (d.directory && d.symlink) it.disable_recursion_pending();
        switch (visit(d)) {
            case WalkStep::next:     break;
            case WalkStep::skip_dir: if (d.directory) it.disable_recursion_pending(); break;
            case WalkStep::stop:     return true;
        }
    }
    return true;
}

bool DiskFiles::create_directories(const fs::path& p, std::error_code& ec) {
    return fs::create_directories(p, ec);
}

bool DiskFiles::remove(const fs::path& p, std::error_code& ec) {
    return fs::remove(p, ec);
}

std::uintmax_t DiskFiles::remove_all(const fs::path& p, std::error_code& ec) {
    return fs::remove_all(p, ec);
}

void DiskFiles::rename(const fs::path& from, const fs::path& to, std::error_code& ec) {
    fs::rename(from, to, ec);
}

void DiskFiles::copy(const fs::path& from, const fs::path& to, std::error_code& ec) {
    // Links are copied as links, never followed into what they point at.
    fs::copy(from, to, fs::copy_options::recursive | fs::copy_options::copy_symlinks, ec);
}

}  // namespace agentty::tools::util
