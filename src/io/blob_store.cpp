// agentty::blobs — content-addressed payload store. See blob_store.hpp.

#include "agentty/io/blob_store.hpp"

#include "agentty/io/persistence.hpp"   // threads_dir, write_json_atomic

#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iterator>

namespace agentty::blobs {

namespace fs = std::filesystem;

fs::path dir() {
    auto p = persistence::threads_dir() / "blobs";
    std::error_code ec;
    fs::create_directories(p, ec);
    return p;
}

std::string name_for(std::string_view bytes) {
    // FNV-1a over the bytes. Not cryptographic — this names a local cache
    // entry, it doesn't authenticate anything. 64 bits over a few thousand
    // payloads is a collision probability far below the disk's own error
    // rate, and the length is mixed into the name as a cheap second
    // dimension.
    //
    // The exact format ("%016llx-%zx") is load-bearing: every blob already
    // on disk is named by it, so changing it would orphan every image in
    // every existing thread. Do not "clean this up".
    std::uint64_t h = 1469598103934665603ull;
    for (unsigned char c : bytes) {
        h ^= c;
        h *= 1099511628211ull;
    }
    char buf[40];
    std::snprintf(buf, sizeof(buf), "%016llx-%zx",
                  static_cast<unsigned long long>(h), bytes.size());
    return buf;
}

std::string put(const std::string& bytes) {
    if (bytes.empty()) return {};
    const std::string n = name_for(bytes);
    const fs::path out = dir() / n;
    std::error_code ec;
    // Content-addressed: if it's already there it is byte-identical by
    // construction, so rewriting it would be pure I/O for no change.
    if (fs::exists(out, ec)) return n;
    // Reuses the atomic temp+fsync+rename writer: a blob is only ever
    // published complete, so a crash mid-write can't leave a reference
    // pointing at a truncated payload.
    if (!persistence::write_json_atomic(out, bytes)) return {};
    return n;
}

std::string get(const std::string& name) {
    if (name.empty()) return {};
    // Defend the join: a thread file is user-writable, and a crafted
    // "../../" name must not read outside the blob directory.
    if (name.find('/') != std::string::npos
        || name.find('\\') != std::string::npos
        || name.find("..") != std::string::npos) return {};
    std::ifstream in(dir() / name, std::ios::binary);
    if (!in) return {};
    return std::string{std::istreambuf_iterator<char>(in),
                       std::istreambuf_iterator<char>()};
}

} // namespace agentty::blobs
