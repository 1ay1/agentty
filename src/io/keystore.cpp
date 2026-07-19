#include "agentty/auth/keystore.hpp"

#include "agentty/tool/util/subprocess.hpp"

#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#if !defined(_WIN32)
#  include <fcntl.h>
#  include <poll.h>
#  include <signal.h>
#  include <sys/wait.h>
#  include <unistd.h>
#  if __has_include(<spawn.h>)
#    include <spawn.h>
#    define AGENTTY_KS_HAVE_SPAWN 1
#  else
#    define AGENTTY_KS_HAVE_SPAWN 0
#  endif
extern char** environ;
#else
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#  include <wincred.h>
#  ifdef _MSC_VER
#    pragma comment(lib, "advapi32.lib")
#  endif
#endif

namespace agentty::auth::keystore {

namespace {

using tools::util::run_argv_s;
using tools::util::SubprocessResult;

// The Secret Service item is identified by a stable (attribute → value) pair.
// We use a single "service" attribute so lookups/deletes match exactly what
// store wrote. Label is cosmetic (what the keyring UI shows).
constexpr const char* kAttrKey   = "service";
constexpr const char* kLabel     = "agentty credentials";

// ── Truthy env gate ────────────────────────────────────────────────────
bool env_enabled() {
    const char* v = std::getenv("AGENTTY_USE_KEYSTORE");
    return v && *v && std::strcmp(v, "0") != 0 && std::strcmp(v, "false") != 0;
}

// Can we invoke `<exe> <probe-arg>` at all? Used to detect secret-tool /
// security presence without a PATH walk. Exit status is irrelevant — a
// clean spawn (started==true) means the binary exists.
bool can_invoke(const std::vector<std::string>& argv) {
    auto r = run_argv_s(argv, 4096, std::chrono::seconds{5});
    return r.started;
}

enum class Backend { None, LibSecret, MacKeychain, WinCred };

Backend detect_backend() {
#if defined(__APPLE__)
    if (can_invoke({"security", "help"})) return Backend::MacKeychain;
    return Backend::None;
#elif defined(__linux__)
    // `secret-tool --help` returns non-zero but spawns cleanly when present.
    if (can_invoke({"secret-tool", "--help"})) return Backend::LibSecret;
    return Backend::None;
#elif defined(_WIN32)
    // Windows Credential Manager is always present on supported Windows.
    return Backend::WinCred;
#else
    return Backend::None;
#endif
}

Backend backend() {
    static const Backend b = detect_backend();
    return b;
}

#if !defined(_WIN32)
// Spawn argv, write `input` to its stdin, discard stdout/stderr, and return
// the exit code (or -1 on spawn failure). Used for `secret-tool store`, which
// reads the secret from stdin — so the secret never appears in the process
// table / argv the way a `-w <secret>` flag would. Best-effort, short-lived.
int spawn_feed_stdin(const std::vector<std::string>& argv, const std::string& input) {
#if AGENTTY_KS_HAVE_SPAWN
    int in_pipe[2];
    if (::pipe(in_pipe) != 0) return -1;

    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, in_pipe[0], STDIN_FILENO);
    posix_spawn_file_actions_addclose(&fa, in_pipe[1]);
    // Silence the child's output.
    posix_spawn_file_actions_addopen(&fa, STDOUT_FILENO, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_addopen(&fa, STDERR_FILENO, "/dev/null", O_WRONLY, 0);

    // Detach the child into its own session so it has NO controlling terminal.
    // macOS `security -w` reads the password via readpassphrase(), which opens
    // /dev/tty when a controlling tty exists and would otherwise ignore our
    // stdin pipe (and hang). With no tty, readpassphrase() falls back to stdin,
    // which is exactly the pipe we feed the secret through. POSIX_SPAWN_SETSID
    // is macOS 10.15+/glibc 2.26+; harmless for `secret-tool` too.
    posix_spawnattr_t attr;
    posix_spawnattr_init(&attr);
#ifdef POSIX_SPAWN_SETSID
    posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETSID);
#endif

    std::vector<char*> cargv;
    cargv.reserve(argv.size() + 1);
    for (const auto& a : argv) cargv.push_back(const_cast<char*>(a.c_str()));
    cargv.push_back(nullptr);

    pid_t pid = 0;
    int rc = ::posix_spawnp(&pid, cargv[0], &fa, &attr, cargv.data(), environ);
    posix_spawnattr_destroy(&attr);
    posix_spawn_file_actions_destroy(&fa);
    ::close(in_pipe[0]);
    if (rc != 0) { ::close(in_pipe[1]); return -1; }

    // Write the secret, ignoring SIGPIPE if the child died early.
    signal(SIGPIPE, SIG_IGN);
    const char* p = input.data();
    size_t remaining = input.size();
    while (remaining > 0) {
        ssize_t n = ::write(in_pipe[1], p, remaining);
        if (n < 0) { if (errno == EINTR) continue; break; }
        p += n; remaining -= (size_t)n;
    }
    ::close(in_pipe[1]);

    int status = 0;
    while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
#else
    (void)argv; (void)input;
    return -1;
#endif
}
#endif // !_WIN32

#if defined(_WIN32)
// The credential target name shown in Windows Credential Manager. One item
// per logical key, namespaced so we never collide with other apps.
std::wstring win_target(const std::string& key) {
    std::string t = "agentty:" + key;
    int n = MultiByteToWideChar(CP_UTF8, 0, t.c_str(), -1, nullptr, 0);
    std::wstring w(n > 0 ? n - 1 : 0, L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, t.c_str(), -1, w.data(), n);
    return w;
}
#endif

} // namespace

bool available() {
    static const bool ok = env_enabled() && backend() != Backend::None;
    return ok;
}

std::string backend_name() {
    if (!env_enabled()) return "disabled";
    switch (backend()) {
        case Backend::LibSecret:   return "libsecret";
        case Backend::MacKeychain: return "macos-keychain";
        case Backend::WinCred:     return "windows-credential-manager";
        default:                   return "unavailable";
    }
}

Status store(const std::string& key, const std::string& secret) {
    if (!available()) return Status::Unsupported;
    switch (backend()) {
#if !defined(_WIN32)
        case Backend::LibSecret: {
            // secret-tool store --label=<label> <attr> <key>  (secret on stdin)
            int rc = spawn_feed_stdin(
                {"secret-tool", "store", std::string("--label=") + kLabel,
                 kAttrKey, key},
                secret);
            return rc == 0 ? Status::Ok : Status::Error;
        }
        case Backend::MacKeychain: {
            // Replace any existing item first (add fails with dup otherwise),
            // then add. CRITICAL: pass `-w` with NO value so `security` prompts
            // for the password on stdin ("password data for new item:" then
            // "retype password for new item:") instead of taking it from argv,
            // where it would be visible in `ps`/the process table. We feed the
            // secret twice (newline-separated) to satisfy the confirm prompt.
            (void)run_argv_s({"security", "delete-generic-password",
                        "-s", key, "-a", "agentty"}, 4096, std::chrono::seconds{5});
            int rc = spawn_feed_stdin(
                {"security", "add-generic-password", "-U",
                 "-s", key, "-a", "agentty", "-l", kLabel, "-w"},
                secret + "\n" + secret + "\n");
            return rc == 0 ? Status::Ok : Status::Error;
        }
#else
        case Backend::WinCred: {
            // Windows Credential Manager: a per-user, DPAPI-protected generic
            // credential. CRED_PERSIST_LOCAL_MACHINE keeps it on this box only.
            std::wstring target = win_target(key);
            CREDENTIALW cred{};
            cred.Type               = CRED_TYPE_GENERIC;
            cred.TargetName         = const_cast<LPWSTR>(target.c_str());
            cred.CredentialBlobSize = static_cast<DWORD>(secret.size());
            cred.CredentialBlob     = reinterpret_cast<LPBYTE>(
                const_cast<char*>(secret.data()));
            cred.Persist            = CRED_PERSIST_LOCAL_MACHINE;
            std::wstring user = L"agentty";
            cred.UserName           = const_cast<LPWSTR>(user.c_str());
            return CredWriteW(&cred, 0) ? Status::Ok : Status::Error;
        }
#endif
        default: return Status::Unsupported;
    }
}

Status retrieve(const std::string& key, std::string& out) {
    if (!available()) return Status::Unsupported;
    switch (backend()) {
#if !defined(_WIN32)
        case Backend::LibSecret: {
            auto r = run_argv_s({"secret-tool", "lookup", kAttrKey, key},
                                1u << 20, std::chrono::seconds{10});
            if (!r.started) return Status::Error;
            if (r.exit_code != 0 || r.output.empty()) return Status::NotFound;
            // secret-tool prints the secret with no trailing newline; but strip
            // one defensively in case the platform adds it.
            out = r.output;
            if (!out.empty() && out.back() == '\n') out.pop_back();
            return Status::Ok;
        }
        case Backend::MacKeychain: {
            auto r = run_argv_s(
                {"security", "find-generic-password", "-s", key,
                 "-a", "agentty", "-w"},
                1u << 20, std::chrono::seconds{10});
            if (!r.started) return Status::Error;
            if (r.exit_code != 0 || r.output.empty()) return Status::NotFound;
            out = r.output;
            if (!out.empty() && out.back() == '\n') out.pop_back();
            return Status::Ok;
        }
#else
        case Backend::WinCred: {
            std::wstring target = win_target(key);
            PCREDENTIALW cred = nullptr;
            if (!CredReadW(target.c_str(), CRED_TYPE_GENERIC, 0, &cred)) {
                DWORD e = GetLastError();
                return (e == ERROR_NOT_FOUND) ? Status::NotFound : Status::Error;
            }
            out.assign(reinterpret_cast<const char*>(cred->CredentialBlob),
                       cred->CredentialBlobSize);
            CredFree(cred);
            return Status::Ok;
        }
#endif
        default: return Status::Unsupported;
    }
}

Status remove(const std::string& key) {
    if (!available()) return Status::Unsupported;
    switch (backend()) {
#if !defined(_WIN32)
        case Backend::LibSecret: {
            auto r = run_argv_s({"secret-tool", "clear", kAttrKey, key},
                                4096, std::chrono::seconds{5});
            return r.started ? Status::Ok : Status::Error;
        }
        case Backend::MacKeychain: {
            auto r = run_argv_s({"security", "delete-generic-password",
                                 "-s", key, "-a", "agentty"},
                                4096, std::chrono::seconds{5});
            // exit 44 == item not found; treat as NotFound, else Ok.
            if (r.started && r.exit_code == 44) return Status::NotFound;
            return r.started ? Status::Ok : Status::Error;
        }
#else
        case Backend::WinCred: {
            std::wstring target = win_target(key);
            if (!CredDeleteW(target.c_str(), CRED_TYPE_GENERIC, 0)) {
                DWORD e = GetLastError();
                return (e == ERROR_NOT_FOUND) ? Status::NotFound : Status::Error;
            }
            return Status::Ok;
        }
#endif
        default: return Status::Unsupported;
    }
}

} // namespace agentty::auth::keystore
