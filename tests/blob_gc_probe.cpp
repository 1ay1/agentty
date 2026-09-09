// blob_gc_probe — run the sweep over a real threads directory.
//
// The unit tests use synthetic stores. This runs the same code over an
// actual ~/.agentty/threads, where the reference shapes are whatever
// three years of format evolution produced.
//
//   ./build/agentty_standalone_tests blob_gc_probe <threads-dir> [--apply]
//
// DRY RUN BY DEFAULT. It reports what would be reclaimed and touches
// nothing; --apply is required to delete. That asymmetry is deliberate —
// the files at stake hold the user's images and tool output.
//
// No argument = no-op pass, so it costs nothing in CI.

#include <agentty/io/blob_gc.hpp>

#include <cstdio>
#include <filesystem>
#include <string>

namespace fs = std::filesystem;
using namespace agentty;

int main(int argc, char** argv) {
    fs::path dir;
    bool apply = false;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--apply") apply = true;
        else                dir = a;
    }
    if (dir.empty()) {
        std::printf("blob_gc_probe: no directory given, nothing to do.\n"
                    "usage: %s <threads-dir> [--apply]\n", argv[0]);
        return 0;
    }
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) {
        std::printf("FAIL: %s is not a directory\n", dir.string().c_str());
        return 1;
    }

    const auto st = blobs::collect_in(dir, /*dry_run=*/!apply);

    std::printf("%s\n", dir.string().c_str());
    std::printf("  threads scanned  %6zu\n", st.scanned_threads);
    std::printf("  blobs on disk    %6zu\n", st.total_blobs);
    std::printf("  referenced       %6zu\n", st.referenced);
    std::printf("  orphaned         %6zu  (%.1f MB)\n",
                st.deleted, static_cast<double>(st.bytes_freed) / 1e6);
    if (st.dangling)
        std::printf("  DANGLING REFS    %6zu  <- referenced but missing\n",
                    st.dangling);

    if (!st.ran) {
        std::printf("\nABORTED: %zu thread file(s) unreadable — deleted nothing.\n"
                    "A thread whose references are unknown must not have its "
                    "payloads reclaimed.\n", st.unreadable);
        return 1;
    }
    std::printf("\n%s\n", apply ? "APPLIED: orphans deleted."
                                : "DRY RUN: nothing was deleted. "
                                  "Pass --apply to reclaim.");
    return 0;
}
