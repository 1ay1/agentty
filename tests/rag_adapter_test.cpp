// rag_adapter_test — locks the agentty↔rag-cpp adapter contract.
//
// agentty's retrieval engine is the external rag-cpp library (rag::Engine),
// driven through the compact agentty::rag::Retriever boundary in
// include/agentty/rag/rag_adapter.hpp. This test drives that REAL boundary
// end to end, fully OFFLINE: an unreachable Ollama endpoint is probed once and
// the adapter runs BM25 without repeated network timeouts.
//
// It pins the properties the rest of agentty depends on:
//   1. A docs folder is indexed and a relevant query returns ranked passages
//      whose `source` is "docs" and whose `path` is the file (provenance
//      survives the uri round-trip).
//   2. The top passage for a pointed query is the file that actually contains
//      the answer (ranking is not random).
//   3. An empty knowledge set reports the "no knowledge configured" error
//      instead of throwing or returning garbage.
//   4. warm()/retrieve() are safe to call repeatedly (idempotent reindex).
//   5. retrieve_code() indexes the cwd source tree and finds a symbol.

#include "agentty/rag/rag_adapter.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

namespace fs = std::filesystem;

static int g_fails = 0;
static void check(bool ok, const char* what) {
    std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++g_fails;
}

static void write_file(const fs::path& p, const std::string& body) {
    fs::create_directories(p.parent_path());
    std::ofstream(p) << body;
}

int main() {
    std::printf("rag_adapter_test\n");

    // Isolated temp workspace: a docs/ folder + an env pointing at it.
    auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    fs::path tmp = fs::temp_directory_path() /
                   ("agentty_rag_" + std::to_string(nonce));
    fs::path docs = tmp / "docs";
    fs::remove_all(tmp);
    write_file(docs / "auth.md",
               "# Authentication\n\n"
               "agentty stores OAuth credentials in an encrypted keystore. "
               "The token is refreshed automatically before every request. "
               "To log in run `agentty login` which opens the browser flow.\n");
    write_file(docs / "sandbox.md",
               "# Filesystem sandbox\n\n"
               "Every tool call is confined to the workspace root. Writes "
               "outside the project directory are refused by the sandbox "
               "boundary unless the path is explicitly allowlisted.\n");
    write_file(docs / "build.md",
               "# Building\n\n"
               "Run cmake to configure, then cmake --build to compile the "
               "binary. The test suite runs under ctest.\n");
    for (int i = 0; i < 8; ++i)
        write_file(docs / ("survey" + std::to_string(i) + ".md"),
                   "# Survey facet " + std::to_string(i) + "\n\n"
                   "orion broad survey shared topic facet" + std::to_string(i) + "\n");
    std::string huge = "# Giant reference\n";
    for (int i = 0; i < 3000; ++i)
        huge += "unrelated filler line " + std::to_string(i) + "\n";
    huge += "needle-budget exact relevant sentence\n";
    write_file(docs / "giant.md", huge);
    write_file(tmp / "src" / "auth_guard.cpp",
               "bool validate_bearer_token(const std::string& token) {\n"
               "  return token == \"valid\";\n}\n");
    auto old_cwd = fs::current_path();
    fs::current_path(tmp);

#if defined(_WIN32)
    _putenv_s("AGENTTY_DOCS_DIR", docs.string().c_str());
    // Force the offline path: one bounded probe, then BM25-only retrieval.
    _putenv_s("AGENTTY_OLLAMA_HOST", "127.0.0.1:1");
    // Isolate ranking from the AMBIENT environment: the developer's installed
    // skills and learned memory would otherwise be indexed alongside the tiny
    // temp docs and can out-rank them, and a stale .ragdb / feedback TSV under
    // the repo's own .agentty/ would perturb results. Disable both knowledge
    // sources, persistence, and the learning loop for the deterministic
    // ranking assertions.
    _putenv_s("AGENTTY_RAG_SKILLS", "0");
    _putenv_s("AGENTTY_RAG_MEMORY", "0");
    _putenv_s("AGENTTY_RAG_PERSIST", "1");
    _putenv_s("AGENTTY_RAG_LEARN", "0");
    _putenv_s("AGENTTY_RAG_GRAPH", "0");
    _putenv_s("AGENTTY_RAG_PRF", "0");
#else
    ::setenv("AGENTTY_DOCS_DIR", docs.string().c_str(), 1);
    ::setenv("AGENTTY_OLLAMA_HOST", "127.0.0.1:1", 1);   // bounded probe, then BM25
    ::setenv("AGENTTY_RAG_SKILLS", "0", 1);
    ::setenv("AGENTTY_RAG_MEMORY", "0", 1);
    ::setenv("AGENTTY_RAG_PERSIST", "1", 1);
    ::setenv("AGENTTY_RAG_LEARN", "0", 1);
    ::setenv("AGENTTY_RAG_GRAPH", "0", 1);
    ::setenv("AGENTTY_RAG_PRF", "0", 1);
#endif

    {
        agentty::rag::Retriever r;

        // (1)+(2): a pointed query returns docs-sourced, well-ranked passages.
        auto res = r.retrieve("how do I log in / authenticate", 5, /*skip_docs=*/false);
        check(res.error.empty(), "retrieve() succeeds on a populated docs folder");
        check(!res.passages.empty(), "retrieve() returns at least one passage");
        if (!res.passages.empty()) {
            const auto& top = res.passages.front();
            check(top.source == "docs", "top passage is source-tagged \"docs\"");
            check(top.path.find("auth") != std::string::npos,
                  "top passage for an auth query is auth.md (ranking works)");
            check(!top.text.empty(), "passage carries body text");
            check(!res.mode.empty(), "mode/provenance string is populated");
        }

        // (4): repeat calls are safe and stay warm (no reindex churn / crash).
        auto res2 = r.retrieve("filesystem sandbox workspace root", 3);
        check(res2.error.empty(), "second retrieve() succeeds");
        check(!res2.passages.empty(), "second query returns passages");
        if (!res2.passages.empty())
            check(res2.passages.front().path.find("sandbox") != std::string::npos,
                  "sandbox query ranks sandbox.md first");
        check(r.warm(), "index reports warm after a build");

        // Full-power features engage: the mode string advertises the rich
        // pipeline as a readable FUNNEL (fusion method + per-stage counts) and
        // CRAG produced a confidence.
        {
            auto q = r.retrieve("how do I log in / authenticate", 5);
            check(q.mode.find("convex-fusion") != std::string::npos,
                  "mode advertises convex (TM2C2) fusion (rag-cpp default)");
            check(q.mode.find("funnel:") != std::string::npos,
                  "mode renders the retrieval funnel (engine workings are shown)");
            check(q.mode.find("confidence") != std::string::npos,
                  "mode reports a confidence signal");
            check(q.confidence >= 0.0 && q.confidence <= 1.0,
                  "confidence is a well-formed [0,1] signal");
        }

        // A real persisted index and validation manifest must be written.
        {
            std::error_code ec;
            auto ragdb = tmp / ".agentty" / "rag_docs.ragdb";
            auto meta = fs::path{ragdb.string() + ".meta.json"};
            check(fs::is_regular_file(ragdb, ec), "persisted .ragdb is written");
            check(fs::is_regular_file(meta, ec), "persisted source manifest is written");
        }

        // Requested breadth is honored; corrective retrieval must not silently
        // collapse a broad k=8 request to its old three-strip default.
        {
            auto broad = r.retrieve("orion broad survey shared topic", 8);
            check(broad.error.empty(), "broad retrieval succeeds");
            check(broad.passages.size() >= 6, "broad retrieval is not capped at three passages");
        }

        // Aggregate body output is bounded near 12 KiB and keeps the relevant
        // span from the tail of a giant source.
        {
            auto bounded = r.retrieve("needle-budget exact relevant sentence", 6);
            std::size_t bytes = 0;
            bool kept_needle = false;
            for (const auto& p : bounded.passages) {
                bytes += p.text.size();
                kept_needle = kept_needle || p.text.find("needle-budget") != std::string::npos;
            }
            check(bytes <= 12 * 1024, "retrieval passage bodies obey aggregate budget");
            check(kept_needle, "query-focused compression keeps the relevant span");
        }

        // Generator seam is callable and drives HyDE when enabled.
        {
#if defined(_WIN32)
            _putenv_s("AGENTTY_RAG_HYDE", "1");
#else
            ::setenv("AGENTTY_RAG_HYDE", "1", 1);
#endif
            bool gen_called = false;
            agentty::rag::Retriever r2;
            r2.set_generator([&](const std::string&, int n) {
                gen_called = true;
                std::vector<std::string> outs;
                for (int i = 0; i < (n > 0 ? n : 1); ++i)
                    outs.push_back("authenticate login oauth token browser flow");
                return outs;
            });
            auto hy = r2.retrieve("how to authenticate", 5);
            check(hy.error.empty(), "HyDE-enabled retrieve succeeds");
            check(gen_called, "generator seam is invoked when HyDE is on");
#if defined(_WIN32)
            _putenv_s("AGENTTY_RAG_HYDE", "0");
#else
            ::setenv("AGENTTY_RAG_HYDE", "0", 1);
#endif
        }

        // Learning is intentionally opt-in: merely surfacing results must not
        // create feedback that systematically penalizes skills/memory.
        {
            std::error_code ec;
            auto fb = tmp / ".agentty" / "rag_feedback.tsv";
            fs::remove(fb, ec);
            (void)r.retrieve("filesystem sandbox workspace root", 3);
            agentty::rag::feedback::note_file_opened("sandbox.md");
            check(!fs::exists(fb, ec), "implicit learning is disabled by default");
        }

        // Source-aware code index returns a definition-shaped chunk and updates
        // one changed file without discarding the whole corpus.
        {
            auto code = r.retrieve_code("validate bearer token credentials", 5);
            check(code.error.empty() && !code.passages.empty(), "search_code finds source");
            if (!code.passages.empty()) {
                check(code.passages.front().path.find("auth_guard.cpp") != std::string::npos,
                      "code result points at auth_guard.cpp");
                check(code.passages.front().text.find("validate_bearer_token") != std::string::npos,
                      "code-aware chunk preserves the function definition");
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            write_file(tmp / "src" / "auth_guard.cpp",
                       "bool rotate_session_nonce(int nonce) { return nonce > 41; }\n");
            auto updated = r.retrieve_code("rotate session nonce", 5);
            check(updated.error.empty() && !updated.passages.empty(),
                  "incremental code refresh finds an edited file");
            if (!updated.passages.empty())
                check(updated.passages.front().text.find("rotate_session_nonce") != std::string::npos,
                      "edited definition replaces stale code content");
        }
        // A single documentation edit is refreshed in place and replaces the
        // stale document without rebuilding unrelated sources.
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            write_file(docs / "auth.md",
                       "# Authentication\n\nEncrypted OAuth keystore and browser login. "
                       "lattice-refresh-marker is now documented.\n");
            auto updated = r.retrieve("lattice refresh marker", 5);
            check(updated.error.empty() && !updated.passages.empty(),
                  "incremental docs refresh finds an edited document");
            if (!updated.passages.empty())
                check(updated.passages.front().text.find("lattice-refresh-marker") != std::string::npos,
                      "edited docs content replaces the stale passage");
        }
    }

    // A fresh Retriever opens the persisted corpus without rewriting it.
    {
        auto db = tmp / ".agentty" / "rag_docs.ragdb";
        std::error_code ec;
        auto before = fs::last_write_time(db, ec);
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        agentty::rag::Retriever warm;
        auto res = warm.retrieve("encrypted OAuth keystore", 5);
        auto after = fs::last_write_time(db, ec);
        check(res.error.empty() && !res.passages.empty(), "fresh retriever opens persisted index");
        check(before == after, "warm open does not rewrite the persisted index");
        if (!res.passages.empty())
            check(res.passages.front().path.find("auth") != std::string::npos,
                  "warm-opened index preserves ranking");

        auto code_db = tmp / ".agentty" / "rag_code.ragdb";
        auto code_before = fs::last_write_time(code_db, ec);
        auto code = warm.retrieve_code("rotate session nonce", 5);
        auto code_after = fs::last_write_time(code_db, ec);
        check(code.error.empty() && !code.passages.empty(),
              "fresh retriever opens persisted code index");
        check(code_before == code_after, "warm code open does not rewrite its index");
    }

    // Very-large-corpus bounds: a docs root with more than the file cap,
    // plus vendored/build dirs and an oversized blob, must index a BOUNDED
    // corpus and never crash. This guards the OOM that unbounded DirOptions
    // (max_files=0, 4 MB files, no _deps/build exclusion) caused on large
    // codebases when AGENTTY_DOCS_DIR pointed at a repo root.
    {
        fs::path big = tmp / "big_corpus";
        fs::remove_all(big);
        // 7000 tiny docs (> the 6000 cap) so the walk must truncate.
        for (int i = 0; i < 7000; ++i)
            write_file(big / ("doc" + std::to_string(i) + ".md"),
                       "# Doc " + std::to_string(i) +
                       "\n\nalpha beta gamma delta term" + std::to_string(i) + "\n");
        // Vendored + build output that MUST be skipped, not indexed.
        write_file(big / "_deps" / "vendored.md", "should not be indexed\n");
        write_file(big / "build" / "generated.md", "should not be indexed\n");
        write_file(big / "node_modules" / "pkg.md", "should not be indexed\n");
        // An oversized blob that exceeds the 1 MB per-file cap.
        std::string blob = "# Huge\n";
        blob.reserve(3 * 1024 * 1024);
        while (blob.size() < 3u * 1024 * 1024) blob += "filler filler filler\n";
        write_file(big / "huge.md", blob);

#if defined(_WIN32)
        _putenv_s("AGENTTY_DOCS_DIR", big.string().c_str());
#else
        ::setenv("AGENTTY_DOCS_DIR", big.string().c_str(), 1);
#endif
        agentty::rag::Retriever r;
        auto res = r.retrieve("alpha beta gamma delta", 5);
        check(res.error.empty(), "large-corpus retrieve() does not crash or error");
        check(!res.passages.empty(), "large-corpus retrieve() returns passages");
        for (const auto& p : res.passages) {
            check(p.path.find("_deps") == std::string::npos
                  && p.path.find("node_modules") == std::string::npos,
                  "vendored/build dirs are excluded from the docs corpus");
        }
    }

    // (3): empty knowledge ⇒ graceful "no knowledge" error, not a crash.
    {
        fs::path empty_dir = tmp / "empty";
        fs::create_directories(empty_dir);
#if defined(_WIN32)
        _putenv_s("AGENTTY_DOCS_DIR", empty_dir.string().c_str());
        _putenv_s("AGENTTY_RAG_SKILLS", "0");
        _putenv_s("AGENTTY_RAG_MEMORY", "0");
#else
        ::setenv("AGENTTY_DOCS_DIR", empty_dir.string().c_str(), 1);
        ::setenv("AGENTTY_RAG_SKILLS", "0", 1);
        ::setenv("AGENTTY_RAG_MEMORY", "0", 1);
#endif
        agentty::rag::Retriever r;
        auto res = r.retrieve("anything at all", 5);
        check(!res.error.empty(), "empty knowledge set reports an error");
        check(res.passages.empty(), "empty knowledge set returns no passages");
    }

    fs::current_path(old_cwd);
    fs::remove_all(tmp);

    std::printf("%s\n", g_fails == 0 ? "ALL PASS" : "FAILURES");
    return g_fails == 0 ? 0 : 1;
}
