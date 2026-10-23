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

#include "agtest.hpp"

#include "agentty/rag/rag_adapter.hpp"
#include "agentty/tool/util/fs_helpers.hpp"   // workspace_root / set_workspace_root

#include <chrono>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#if defined(_WIN32)
#  include <winsock2.h>
#  include <ws2tcpip.h>
#else
#  include <arpa/inet.h>
#  include <netinet/in.h>
#  include <sys/socket.h>
#  include <sys/time.h>
#  include <unistd.h>
#endif

namespace fs = std::filesystem;


static void write_file(const fs::path& p, const std::string& body) {
    fs::create_directories(p.parent_path());
    std::ofstream(p) << body;
}

namespace {

using json = nlohmann::json;
#if defined(_WIN32)
using socket_t = SOCKET;
constexpr socket_t kInvalidSocket = INVALID_SOCKET;
#else
using socket_t = int;
constexpr socket_t kInvalidSocket = -1;
#endif

void close_socket(socket_t s) noexcept {
    if (s == kInvalidSocket) return;
#if defined(_WIN32)
    ::closesocket(s);
#else
    ::close(s);
#endif
}

class LoopbackEmbeddingServer {
public:
    // `delay`: how long each request takes, so a test can see requests
    // overlap (and what else is possible meanwhile).
    explicit LoopbackEmbeddingServer(std::chrono::milliseconds delay = std::chrono::milliseconds{0})
        : delay_(delay) {
#if defined(_WIN32)
        WSADATA data{};
        if (::WSAStartup(MAKEWORD(2, 2), &data) != 0) return;
        winsock_started_ = true;
#endif
        listener_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (listener_ == kInvalidSocket) return;
        int one = 1;
        (void)::setsockopt(listener_, SOL_SOCKET, SO_REUSEADDR,
                           reinterpret_cast<const char*>(&one), sizeof(one));
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;
        if (::bind(listener_, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) != 0
            || ::listen(listener_, 16) != 0) {
            close_socket(listener_);
            listener_ = kInvalidSocket;
            return;
        }
#if defined(_WIN32)
        int len = sizeof(addr);
#else
        socklen_t len = sizeof(addr);
#endif
        if (::getsockname(listener_, reinterpret_cast<sockaddr*>(&addr), &len) != 0) {
            close_socket(listener_);
            listener_ = kInvalidSocket;
            return;
        }
        port_ = ntohs(addr.sin_port);
        worker_ = std::thread([this] { serve(); });
    }

    ~LoopbackEmbeddingServer() {
        stop_.store(true, std::memory_order_release);
        if (listener_ != kInvalidSocket) {
            sockaddr_in addr{};
            addr.sin_family = AF_INET;
            addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            addr.sin_port = htons(port_);
            socket_t wake = ::socket(AF_INET, SOCK_STREAM, 0);
            if (wake != kInvalidSocket) {
                (void)::connect(wake, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr));
                close_socket(wake);
            }
        }
        if (worker_.joinable()) worker_.join();
        // Each connection runs on its own thread; wait for the ones in flight.
        for (auto& t : handlers_)
            if (t.joinable()) t.join();
        close_socket(listener_);
#if defined(_WIN32)
        if (winsock_started_) ::WSACleanup();
#endif
    }

    LoopbackEmbeddingServer(const LoopbackEmbeddingServer&) = delete;
    LoopbackEmbeddingServer& operator=(const LoopbackEmbeddingServer&) = delete;

    [[nodiscard]] bool valid() const noexcept { return listener_ != kInvalidSocket; }
    [[nodiscard]] std::uint16_t port() const noexcept { return port_; }
    [[nodiscard]] int requests() const noexcept { return requests_.load(std::memory_order_acquire); }
    // Most requests ever being answered at the same moment.
    [[nodiscard]] int peak_in_flight() const noexcept { return peak_.load(std::memory_order_acquire); }

private:
    static std::string response_for(const std::string& body) {
        std::size_t count = 1;
        const auto request = json::parse(body, nullptr, false);
        if (request.is_object() && request.contains("input") && request["input"].is_array())
            count = request["input"].size();
        json data = json::array();
        for (std::size_t i = 0; i < count; ++i)
            data.push_back({{"object", "embedding"}, {"index", i},
                            {"embedding", {1.0, 0.0, 0.0, 0.0,
                                             0.0, 0.0, 0.0, 0.0}}});
        const std::string payload = json{{"object", "list"}, {"data", std::move(data)}}.dump();
        return "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nConnection: close\r\n"
               "Content-Length: " + std::to_string(payload.size()) + "\r\n\r\n" + payload;
    }

    static bool send_all(socket_t fd, const std::string& bytes) {
        std::size_t sent = 0;
        while (sent < bytes.size()) {
            const auto left = bytes.size() - sent;
#if defined(_WIN32)
            const int n = ::send(fd, bytes.data() + sent, static_cast<int>(left), 0);
#else
            const auto n = ::send(fd, bytes.data() + sent, left, 0);
#endif
            if (n <= 0) return false;
            sent += static_cast<std::size_t>(n);
        }
        return true;
    }

    void handle(socket_t fd) {
        std::string request;
        char buf[4096];
        std::size_t header_end = std::string::npos;
        std::size_t content_length = 0;
        for (;;) {
#if defined(_WIN32)
            const int n = ::recv(fd, buf, sizeof(buf), 0);
#else
            const auto n = ::recv(fd, buf, sizeof(buf), 0);
#endif
            if (n <= 0) return;
            request.append(buf, static_cast<std::size_t>(n));
            if (header_end == std::string::npos) {
                header_end = request.find("\r\n\r\n");
                if (header_end != std::string::npos) {
                    const auto headers = request.substr(0, header_end);
                    const auto marker = headers.find("Content-Length:");
                    if (marker != std::string::npos)
                        content_length = std::strtoull(headers.c_str() + marker + 15, nullptr, 10);
                }
            }
            if (header_end != std::string::npos
                && request.size() >= header_end + 4 + content_length)
                break;
        }
        requests_.fetch_add(1, std::memory_order_release);
        const int now = in_flight_.fetch_add(1, std::memory_order_acq_rel) + 1;
        int peak = peak_.load(std::memory_order_acquire);
        while (now > peak && !peak_.compare_exchange_weak(peak, now, std::memory_order_acq_rel)) {}
        if (delay_.count() > 0) std::this_thread::sleep_for(delay_);
        const auto body = request.substr(header_end + 4, content_length);
        const auto response = response_for(body);
        (void)send_all(fd, response);
        in_flight_.fetch_sub(1, std::memory_order_acq_rel);
    }

    void serve() {
        while (!stop_.load(std::memory_order_acquire)) {
#if defined(_WIN32)
            const socket_t client = ::accept(listener_, nullptr, nullptr);
#else
            const socket_t client = ::accept(listener_, nullptr, nullptr);
#endif
            if (client == kInvalidSocket) {
                if (stop_.load(std::memory_order_acquire)) break;
                continue;
            }
            // A client that connects and never sends (or a kept-alive socket
            // the client forgot) must not wedge this thread: the destructor
            // joins it, and a stuck join hangs ctest instead of failing.
#if defined(_WIN32)
            const DWORD ms = 5000;
            (void)::setsockopt(client, SOL_SOCKET, SO_RCVTIMEO,
                               reinterpret_cast<const char*>(&ms), sizeof(ms));
#else
            timeval tv{5, 0};
            (void)::setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif
            if (stop_.load(std::memory_order_acquire)) {
                close_socket(client);
                break;
            }
            // A real embedding server answers requests side by side; so does
            // this one, so a test can see the client's concurrency.
            handlers_.emplace_back([this, client] {
                handle(client);
                close_socket(client);
            });
        }
    }

    socket_t listener_ = kInvalidSocket;
    std::uint16_t port_ = 0;
    std::thread worker_;
    std::vector<std::thread> handlers_;   // touched only by worker_, joined after it
    std::atomic<bool> stop_{false};
    std::atomic<int> requests_{0};
    std::atomic<int> in_flight_{0};
    std::atomic<int> peak_{0};
    std::chrono::milliseconds delay_{0};
#if defined(_WIN32)
    bool winsock_started_ = false;
#endif
};

class EnvironmentRestore {
public:
    explicit EnvironmentRestore(std::initializer_list<const char*> names) {
        for (const char* name : names) {
            const char* value = std::getenv(name);
            saved_.emplace_back(name, value ? std::optional<std::string>{value} : std::nullopt);
        }
    }

    ~EnvironmentRestore() {
        for (const auto& [name, value] : saved_) {
#if defined(_WIN32)
            _putenv_s(name.c_str(), value ? value->c_str() : "");
#else
            if (value) ::setenv(name.c_str(), value->c_str(), 1);
            else ::unsetenv(name.c_str());
#endif
        }
    }

    EnvironmentRestore(const EnvironmentRestore&) = delete;
    EnvironmentRestore& operator=(const EnvironmentRestore&) = delete;

private:
    std::vector<std::pair<std::string, std::optional<std::string>>> saved_;
};

void set_test_env(const char* name, const std::string& value) {
#if defined(_WIN32)
    _putenv_s(name, value.c_str());
#else
    ::setenv(name, value.c_str(), 1);
#endif
}

}  // namespace

TEST_CASE("rag cold retrieval serializes fan-out under the index lock") {
    ScopedEnvSandbox sandbox;
    EnvironmentRestore env({
        "AGENTTY_DOCS_DIR", "AGENTTY_OLLAMA_HOST", "AGENTTY_EMBED_BACKEND",
        "AGENTTY_EMBED_ENDPOINT", "AGENTTY_EMBED_MODEL", "AGENTTY_EMBED_PATH",
        "AGENTTY_EMBED_API_KEY", "AGENTTY_RAG_SKILLS", "AGENTTY_RAG_MEMORY",
        "AGENTTY_RAG_MCP", "AGENTTY_RAG_CONTEXTUAL", "AGENTTY_RAG_MMR",
        "AGENTTY_RAG_MMR_LAMBDA", "AGENTTY_RAG_STITCH", "AGENTTY_RAG_DEDUP",
        "AGENTTY_RAG_DEDUP_THRESHOLD", "AGENTTY_RAG_AUTOCUT",
        "AGENTTY_RAG_AUTOCUT_SENSITIVITY", "AGENTTY_RAG_TRACE", "AGENTTY_RAG_PRF",
        "AGENTTY_RAG_CORRECT", "AGENTTY_RAG_GRAPH", "AGENTTY_RAG_EXPAND",
        "AGENTTY_RAG_HYDE", "AGENTTY_RAG_PERSIST", "AGENTTY_RAG_LEARN",
        "AGENTTY_RAG_ADAPTIVE", "AGENTTY_RAG_PROACTIVE", "AGENTTY_RAG_FUSION"});
    // 150 ms a request: long enough that requests visibly overlap when the
    // batches run side by side, and that a reader can get in between while
    // they're in flight; short enough to keep the test under a second or two.
    LoopbackEmbeddingServer server{std::chrono::milliseconds{150}};
    REQUIRE(server.valid(), "loopback embedding server starts");

    TempDir tmp{"rag_fanout"};
    fs::create_directories(tmp.path() / ".git");
    const auto docs = tmp.path() / "docs";
    for (int i = 0; i < 96; ++i) {
        write_file(docs / ("doc" + std::to_string(i) + ".md"),
                   "# Fan-out fixture " + std::to_string(i) + "\n\n"
                   "This document exercises the cold embedding path while the "
                   "RAG index lock is held. fixture-token-" + std::to_string(i) + "\n");
    }
    // cwd and the workspace root are process-global: put both back on every
    // way out, before TempDir removes the directory they point into, or the
    // next test in this binary runs from a deleted directory.
    struct RestoreWorkspace {
        fs::path cwd = fs::current_path();
        fs::path ws  = ::agentty::tools::util::workspace_root();
        ~RestoreWorkspace() {
            std::error_code e;
            fs::current_path(cwd, e);
            ::agentty::tools::util::set_workspace_root(::agentty::IoAccess::grant(), ws);
        }
    };
    // Declared after `tmp`, so it runs first on the way out.
    RestoreWorkspace restore;
    std::error_code ec;
    fs::current_path(tmp.path(), ec);
    REQUIRE(!ec, "temporary project becomes the current directory");
    ::agentty::tools::util::set_workspace_root(::agentty::IoAccess::grant(), tmp.path());

    set_test_env("AGENTTY_DOCS_DIR", docs.string());
    set_test_env("AGENTTY_EMBED_BACKEND", "openai");
    set_test_env("AGENTTY_EMBED_ENDPOINT",
                 "http://127.0.0.1:" + std::to_string(server.port()));
    set_test_env("AGENTTY_EMBED_MODEL", "issue-112-fixture");
    set_test_env("AGENTTY_EMBED_PATH", "/v1/embeddings");
    set_test_env("AGENTTY_EMBED_API_KEY", "test-key");
    for (const char* name : {"AGENTTY_RAG_SKILLS", "AGENTTY_RAG_MEMORY",
                             "AGENTTY_RAG_MCP", "AGENTTY_RAG_CONTEXTUAL",
                             "AGENTTY_RAG_MMR", "AGENTTY_RAG_STITCH",
                             "AGENTTY_RAG_DEDUP", "AGENTTY_RAG_AUTOCUT",
                             "AGENTTY_RAG_TRACE", "AGENTTY_RAG_PRF",
                             "AGENTTY_RAG_CORRECT", "AGENTTY_RAG_GRAPH",
                             "AGENTTY_RAG_EXPAND", "AGENTTY_RAG_HYDE",
                             "AGENTTY_RAG_LEARN", "AGENTTY_RAG_ADAPTIVE",
                             "AGENTTY_RAG_PROACTIVE"})
        set_test_env(name, "0");
    set_test_env("AGENTTY_RAG_PERSIST", "0");

    agentty::rag::Retriever retriever;
    // While the cold retrieve runs, another thread keeps asking a lock-free
    // question that reads the index (the UI's status row does this every
    // frame). With the embedding done under the lock, every one of those
    // would wait out the whole pass.
    std::atomic<bool> done{false};
    std::atomic<int>  answered_during{0};
    std::thread prober([&] {
        while (!done.load(std::memory_order_acquire)) {
            if (server.requests() >= 2) {
                // snapshot_config() takes the index lock for a (blocking)
                // read, so it answers only when no pass holds the lock.
                const auto t0 = std::chrono::steady_clock::now();
                (void)retriever.snapshot_config();
                if (std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds{100})
                    answered_during.fetch_add(1, std::memory_order_relaxed);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds{10});
        }
    });
    const auto result = retriever.retrieve("fixture token cold retrieval", 5);
    done.store(true, std::memory_order_release);
    prober.join();

    check(result.error.empty(), "cold retrieval completes without a lock capability abort");
    check(!result.passages.empty(), "cold retrieval returns passages");
    check(server.requests() >= 4,
          "cold retrieval probes and embeds the corpus in multiple requests");
    check(server.peak_in_flight() >= 2,
          "embedding batches run side by side, not one at a time under the lock");
    check(answered_during.load() > 0,
          "the index stays readable while the corpus is being embedded");

    // Warm: nothing new to embed, so one query vector at most.
    const int before = server.requests();
    const auto again = retriever.retrieve("fixture token cold retrieval", 5);
    check(again.error.empty() && !again.passages.empty(), "a warm retrieval answers");
    check(server.requests() - before <= 1, "a warm retrieval embeds nothing but the query");

    // Switch to another embedder (same model name, another server): the old
    // vectors must not be reused, the index rebuilds on the new one, and the
    // whole cold path runs again without touching the old server.
    LoopbackEmbeddingServer other{std::chrono::milliseconds{20}};
    REQUIRE(other.valid(), "second embedding server starts");
    auto cfg = retriever.snapshot_config();
    cfg.embed.host = "127.0.0.1";
    cfg.embed.port = other.port();
    retriever.apply_config(cfg);
    const int old_before = server.requests();
    const auto switched = retriever.retrieve("fixture token cold retrieval", 5);
    check(switched.error.empty() && !switched.passages.empty(), "retrieval answers after switching embedder");
    check(other.requests() >= 4, "the new embedder re-embeds the corpus");
    check(server.requests() == old_before, "the old embedder is not asked again");
}

// Persisted indexes are named `<stem><8-hex-identity>.ragdb`, where the hash
// identifies the EMBEDDER's vector space (see rag/embed_backend.hpp). Tests
// must not hardcode a name: the identity changes with the embedder, which is
// the whole point — switching backends switches between warm indexes instead
// of silently reopening one built with incompatible geometry.
static fs::path find_index(const fs::path& root, std::string_view stem) {
    std::error_code ec;
    // Indexes live under .agentty/cache/ (derived data, sweepable). The old
    // flat location is checked too so this still passes mid-migration.
    for (const auto& dir : {root / ".agentty" / "cache", root / ".agentty"}) {
        if (!fs::is_directory(dir, ec)) continue;
        for (const auto& e : fs::directory_iterator(dir, ec)) {
            const auto name = e.path().filename().string();
            if (name.rfind(std::string{stem}, 0) == 0
                && name.size() > 6
                && name.compare(name.size() - 6, 6, ".ragdb") == 0)
                return e.path();
        }
    }
    return {};
}

TEST_CASE("rag adapter") {
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
    // Make the sandbox a PROJECT, not just a directory.
    //
    // Persisted indexes resolve through dirs::Root::Project, which walks up
    // from the cwd to the nearest project marker (.git/.agentty/.hg/.svn) --
    // that walk is what gives one checkout one index no matter which subdir
    // you launch from (#61). A markerless temp dir has no marker to find, so
    // the walk ran all the way up and anchored on whatever real repo the test
    // binary happened to be launched from: the 33 MB rag_code.*.ragdb landed
    // in agentty's OWN .agentty/ instead of here, find_index(tmp, ...) found
    // nothing, and the next block threw on `fs::path{""} + ".meta.json"`.
    //
    // That is why this only failed in a FULL run: alone, ctest's cwd already
    // sat inside the repo whose .agentty the index went to, so find_index
    // happened to see a pre-existing one and the assertions passed by
    // accident. The marker makes the sandbox self-contained either way.
    fs::create_directories(tmp / ".git");
    auto old_cwd = fs::current_path();
    fs::current_path(tmp);
    // chdir alone is NOT enough. project_anchor() starts its marker walk at
    // util::project_root(), which is the WORKSPACE ROOT clamp, not the live
    // cwd -- and the workspace root is process-global state that any earlier
    // test in the binary may have pinned somewhere else (several call
    // set_workspace_root and never restore it). So this sandbox has to claim
    // the boundary explicitly, or the anchor resolves into whatever directory
    // the last test left behind and the index lands there.
    const auto old_ws = ::agentty::tools::util::workspace_root();
    ::agentty::tools::util::set_workspace_root(::agentty::IoAccess::grant(), tmp);

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
        //
        // The filename carries the EMBEDDER IDENTITY hash (see
        // embed_backend.hpp): a .ragdb is only reusable by an embedder that
        // produces the same vector geometry, and keying the path on that
        // means switching backends switches between warm indexes instead of
        // silently reopening one built by a different embedder. So this
        // asserts the SHAPE (rag_docs.<8 hex>.ragdb + its manifest) rather
        // than a fixed name.
        {
            std::error_code ec;
            const auto ragdb = find_index(tmp, "rag_docs.");
            check(!ragdb.empty() && fs::is_regular_file(ragdb, ec),
                  "persisted .ragdb is written under an identity-keyed name");
            if (!ragdb.empty()) {
                auto meta = fs::path{ragdb.string() + ".meta.json"};
                check(fs::is_regular_file(meta, ec),
                      "persisted source manifest is written");
            }
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
            agentty::rag::feedback::note_file_opened(agtest::cap(), "sandbox.md");
            check(!fs::exists(fb, ec), "implicit learning is disabled by default");
        }

        // Source-aware code index returns a definition-shaped chunk and updates
        // one changed file without discarding the whole corpus.
        {
            // Gate for OPPORTUNISTIC retrieval (structural zero-hit leads):
            // cold — no in-memory index, no persisted ragdb — must report NOT
            // warm so a side-effect query can't trigger the cold build…
            check(!r.code_warm(), "code index reports cold before first build");
            auto code = r.retrieve_code("validate bearer token credentials", 5);
            check(code.error.empty() && !code.passages.empty(), "search_code finds source");
            // …and warm right after the explicit build.
            check(r.code_warm(), "code index reports warm after explicit build");
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

            // Disk-verification: the returned passage text must be exactly what
            // is on disk RIGHT NOW — read_disk_lines re-reads the cited range, so
            // even after an edit the result is correct-on-bytes, never a stale
            // or mangled index chunk. Prove it by confirming the passage body
            // is a verbatim substring of the current file.
            if (!updated.passages.empty()) {
                std::ifstream in(tmp / "src" / "auth_guard.cpp", std::ios::binary);
                std::string disk((std::istreambuf_iterator<char>(in)),
                                 std::istreambuf_iterator<char>());
                const auto& body = updated.passages.front().text;
                // Take the first non-empty line of the passage and require it
                // to appear verbatim on disk (guards against stale/garbled text).
                std::string first = body.substr(0, body.find('\n'));
                check(!first.empty() && disk.find(first) != std::string::npos,
                      "search_code passage is verified against live disk");
            }
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
            // The code index also covers .md, so refresh it too. Otherwise the
            // warm open below would rightly rewrite a stale code index.
            (void)r.retrieve_code("rotate session nonce", 5);
        }
    }

    // A fresh Retriever opens the persisted corpus without rewriting it.
    {
        auto db = find_index(tmp, "rag_docs.");
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

        auto code_db = find_index(tmp, "rag_code.");
        // Never let a MISSING index degrade into a filesystem exception. The
        // whole suite used to abort here with "cannot create directories:
        // Invalid argument" whenever the index resolved outside `tmp`, which
        // reported as a crash in an unrelated place instead of naming the
        // real problem. A check() says which index is missing and keeps the
        // remaining assertions running.
        REQUIRE(!code_db.empty(),
                "rag_code index exists under the sandbox anchor");
        auto code_before = fs::last_write_time(code_db, ec);
        auto code = warm.retrieve_code("rotate session nonce", 5);
        auto code_after = fs::last_write_time(code_db, ec);
        check(code.error.empty() && !code.passages.empty(),
              "fresh retriever opens persisted code index");
        check(code_before == code_after, "warm code open does not rewrite its index");
    }

    // Files edited while agentty was closed: a fresh Retriever loads the saved
    // index and updates just those files, so new text is found and the rest
    // of the corpus (survey docs here) is still there.
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        write_file(docs / "sandbox.md",
                   "# Sandbox\nThe quokka filter drops writes outside the jail.\n");
        write_file(tmp / "src" / "auth_guard.cpp",
                   "bool quokka_nonce_gate(int nonce) { return nonce > 7; }\n");
        agentty::rag::Retriever warm;
        auto res = warm.retrieve("quokka filter drops writes", 5);
        check(res.error.empty() && !res.passages.empty()
                  && res.passages.front().path.find("sandbox") != std::string::npos,
              "a doc edited while closed is found after reopen");
        auto old = warm.retrieve("encrypted OAuth keystore", 5);
        check(!old.passages.empty()
                  && old.passages.front().path.find("auth") != std::string::npos,
              "unchanged docs survive the incremental update");
        auto code = warm.retrieve_code("quokka nonce gate", 5);
        check(code.error.empty() && !code.passages.empty()
                  && code.passages.front().path.find("auth_guard") != std::string::npos,
              "a code file edited while closed is found after reopen");
        auto stale = warm.retrieve_code("rotate session nonce", 5);
        bool stale_hit = false;
        for (const auto& p : stale.passages)
            if (p.text.find("rotate_session_nonce") != std::string::npos) stale_hit = true;
        check(!stale_hit, "the old text of an edited code file is gone");
    }

    // Corrupt/truncated .meta.json must not crash a fresh Retriever: the load
    // path parses meta inside try/catch and falls back to a clean rebuild. This
    // guards the persistence hardening (atomic meta writes) — even a half-
    // written meta from a killed process is recovered from, never fatal.
    {
        std::error_code ec;
        const auto docs_db = find_index(tmp, "rag_docs.");
        REQUIRE(!docs_db.empty(),
                "rag_docs index exists before the corrupt-meta probe");
        auto meta = fs::path{docs_db.string() + ".meta.json"};
        // Truncated JSON object: exactly what an interrupted write could leave
        // if writes were not atomic.
        write_file(meta, "{\"version\": 3, \"root\": \"/tmp\", \"docs_fp\":");
        agentty::rag::Retriever r;
        auto res = r.retrieve("encrypted OAuth keystore", 5);
        check(res.error.empty(), "corrupt .meta.json recovers via rebuild, no crash");
        check(!res.passages.empty(), "rebuild after corrupt meta still returns results");
        // The rebuild must rewrite a VALID meta (parseable JSON object).
        std::ifstream in(meta);
        std::string body((std::istreambuf_iterator<char>(in)),
                         std::istreambuf_iterator<char>());
        check(!body.empty() && body.front() == '{' && body.back() == '}',
              "rebuild republishes a complete .meta.json");
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
    // Hand the process-global workspace boundary back exactly as we found it:
    // leaving a deleted temp dir pinned is what broke the NEXT test, and this
    // test should not propagate the hazard it just worked around.
    ::agentty::tools::util::set_workspace_root(::agentty::IoAccess::grant(), old_ws);
    fs::remove_all(tmp);
}

// Regression: Retriever::shutdown() must interrupt an in-flight background
// warm PROMPTLY. Before the cooperative-cancel fix, the warm jthread ignored
// its stop_token (captured [state], not [stop_token]) and was only joined at
// static destruction, blocking ^C for 4–10 s while the embed pass finished.
TEST_CASE("rag shutdown interrupts warm promptly") {
    namespace fs = std::filesystem;
    auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    auto tmp = fs::temp_directory_path() /
               ("agentty_rag_shutdown_" + std::to_string(nonce));
    fs::remove_all(tmp);
    fs::create_directories(tmp);
    // Same project marker as the main sandbox above: without it this cwd has
    // no anchor, so anything persisted here resolves into the enclosing real
    // repo's .agentty/ instead.
    fs::create_directories(tmp / ".git");
    auto old_cwd = fs::current_path();
    fs::current_path(tmp);
    // chdir alone is NOT enough. project_anchor() starts its marker walk at
    // util::project_root(), which is the WORKSPACE ROOT clamp, not the live
    // cwd -- and the workspace root is process-global state that any earlier
    // test in the binary may have pinned somewhere else (several call
    // set_workspace_root and never restore it). So this sandbox has to claim
    // the boundary explicitly, or the anchor resolves into whatever directory
    // the last test left behind and the index lands there.
    const auto old_ws = ::agentty::tools::util::workspace_root();
    ::agentty::tools::util::set_workspace_root(::agentty::IoAccess::grant(), tmp);

    // A large-ish corpus so a real warm has work to do (and thus a window in
    // which cancellation matters).
    fs::create_directories(tmp / "docs");
    for (int f = 0; f < 8; ++f) {
        std::ofstream o(tmp / "docs" / ("doc" + std::to_string(f) + ".md"));
        for (int i = 0; i < 400; ++i)
            o << "# section " << f << "-" << i
              << "\nsome searchable prose about topic " << (i % 7) << ".\n\n";
    }

    {
        agentty::rag::Retriever r;
        agentty::rag::Config cfg;
        cfg.docs_root = (tmp / "docs").string();
        r.apply_config(cfg);

        // Kick the background warm, then immediately ask it to stop. The join
        // inside shutdown() must return quickly because the worker now polls
        // the cancel flag between files / before the embed build.
        r.warm_async();
        const auto t0 = std::chrono::steady_clock::now();
        r.shutdown();
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t0).count();
        // Generous ceiling: even one in-flight embed batch is well under this;
        // the OLD bug blocked for the whole multi-second corpus embed.
        check(ms < 2500, "shutdown() returns promptly, not after a full warm");

        // shutdown() is idempotent and leaves the retriever usable.
        r.shutdown();
        r.warm_async();
        r.shutdown();
        check(true, "re-warm + re-shutdown after a cancel is safe");
    }

    fs::current_path(old_cwd);
    // Hand the process-global workspace boundary back exactly as we found it:
    // leaving a deleted temp dir pinned is what broke the NEXT test, and this
    // test should not propagate the hazard it just worked around.
    ::agentty::tools::util::set_workspace_root(::agentty::IoAccess::grant(), old_ws);
    fs::remove_all(tmp);
}
