#include "agentty/auth/keystore.hpp"

#include "agentty/tool/util/subprocess.hpp"

#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#if !defined(_WIN32)
#  include "agentty/tool/util/exec.hpp"   // run_child
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
// [[maybe_unused]]: the Windows backend is WinCred unconditionally and never
// probes an external binary, so this is dead there.
[[maybe_unused]] bool can_invoke(const std::vector<std::string>& argv) {
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
    // run_child puts the child in its own session, so it has no controlling
    // terminal: macOS `security -w` reads via readpassphrase(), which opens
    // /dev/tty when one exists and would ignore our pipe (and hang).
    tools::util::ChildRun run;
    run.argv       = argv;
    run.stdin_data = input;
    run.idle       = std::chrono::seconds{10};
    run.wall       = std::chrono::seconds{30};
    run.max_output_bytes = 4096;   // output is discarded
    const auto r = tools::util::run_child(run);
    return r.started && r.exited ? r.exit_code : -1;
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
