// cred_crypt_test — at-rest credential sealing round-trips.
//
// Each aspect that depends on the process-global passphrase cache
// (resolve_passphrase is call_once) runs in its own forked child, so a single
// test binary can exercise the no-passphrase (v1) and passphrase (v2) states
// independently. On non-fork platforms the passphrase cases are skipped.
#include "agentty/auth/cred_crypt.hpp"

#include <cstdio>
#include <cstdlib>
#include <string>

#if !defined(_WIN32)
#  include <sys/wait.h>
#  include <unistd.h>
#  define HAVE_FORK 1
#else
#  define HAVE_FORK 0
#endif

namespace cc = ::agentty::auth::crypt;

static int g_fail = 0;
#define CHECK(cond, msg)                                                       \
    do {                                                                       \
        if (!(cond)) { std::printf("  FAIL: %s\n", msg); ++g_fail; }           \
        else         { std::printf("  ok:   %s\n", msg); }                     \
    } while (0)

static const std::string kSecret = R"({"method":"oauth","access_token":"sk-abc"})";

// ── Case bodies (each runs with a fixed passphrase env state) ────────────

static int case_v1_machine_only() {
    std::printf("[v1 machine-only]\n");
    CHECK(!cc::passphrase_active(), "passphrase_active() is false");
    auto env = cc::seal(kSecret);
    CHECK(env.has_value(), "seal succeeds");
    CHECK(cc::looks_sealed(*env), "looks_sealed() true");
    CHECK(env->find("\"v\":1") != std::string::npos, "envelope is v1");
    CHECK(env->find("kdf") == std::string::npos, "no kdf field");
    auto pt = cc::unseal(*env);
    CHECK(pt.has_value() && *pt == kSecret, "unseal round-trips");
    // Tamper detection: flip a byte in the ciphertext region.
    std::string bad = *env;
    auto ctpos = bad.find("\"ct\":\"");
    if (ctpos != std::string::npos) { bad[ctpos + 6] = (bad[ctpos + 6] == 'A' ? 'B' : 'A'); }
    CHECK(!cc::unseal(bad).has_value(), "tampered ciphertext rejected");
    // Legacy plaintext is NOT sealed.
    CHECK(!cc::looks_sealed(kSecret), "plaintext not mistaken for sealed");
    return g_fail;
}

static int case_v2_passphrase() {
    std::printf("[v2 passphrase]\n");
    CHECK(cc::passphrase_active(), "passphrase_active() is true");
    auto env = cc::seal(kSecret);
    CHECK(env.has_value(), "seal succeeds");
    CHECK(env->find("\"v\":2") != std::string::npos, "envelope is v2");
    const bool argon = env->find("\"kdf\":\"argon2id\"") != std::string::npos;
    const bool scrypt = env->find("\"kdf\":\"scrypt\"") != std::string::npos;
    CHECK(argon || scrypt, "kdf is argon2id or scrypt");
    std::printf("       (kdf = %s)\n", argon ? "argon2id" : "scrypt");
    auto pt = cc::unseal(*env);
    CHECK(pt.has_value() && *pt == kSecret, "unseal (same pass) round-trips");
    // Emit the envelope so the parent can test cross-state reads.
    if (const char* out = std::getenv("CRED_TEST_OUT")) {
        FILE* f = std::fopen(out, "w");
        if (f) { std::fwrite(env->data(), 1, env->size(), f); std::fclose(f); }
    }
    return g_fail;
}

static int case_v2_read_without_pass(const char* path) {
    std::printf("[v2 read WITHOUT passphrase]\n");
    FILE* f = std::fopen(path, "r");
    if (!f) { std::printf("  FAIL: missing env file\n"); return 1; }
    std::string s; int c; while ((c = std::fgetc(f)) != EOF) s.push_back((char)c);
    std::fclose(f);
    CHECK(!cc::passphrase_active(), "no passphrase in this process");
    CHECK(!cc::unseal(s).has_value(), "v2 file refuses without passphrase");
    return g_fail;
}

static int case_v2_read_wrong_pass(const char* path) {
    std::printf("[v2 read WRONG passphrase]\n");
    FILE* f = std::fopen(path, "r");
    if (!f) { std::printf("  FAIL: missing env file\n"); return 1; }
    std::string s; int c; while ((c = std::fgetc(f)) != EOF) s.push_back((char)c);
    std::fclose(f);
    CHECK(!cc::unseal(s).has_value(), "v2 file with wrong passphrase refuses");
    return g_fail;
}

static int case_v2_read_correct_pass(const char* path) {
    std::printf("[v2 read CORRECT passphrase]\n");
    FILE* f = std::fopen(path, "r");
    if (!f) { std::printf("  FAIL: missing env file\n"); return 1; }
    std::string s; int c; while ((c = std::fgetc(f)) != EOF) s.push_back((char)c);
    std::fclose(f);
    auto pt = cc::unseal(s);
    CHECK(pt.has_value() && *pt == kSecret, "file reopens with correct passphrase");
    return g_fail;
}

#if HAVE_FORK
// Run `fn` in a child that first applies the given env, return its fail count.
static int run_child(void (*setup)(), int (*fn)()) {
    pid_t pid = fork();
    if (pid == 0) {
        setup();
        int rc = fn();
        std::fflush(stdout);
        _exit(rc == 0 ? 0 : 1);
    }
    int status = 0;
    while (waitpid(pid, &status, 0) < 0) {}
    return (WIFEXITED(status) && WEXITSTATUS(status) == 0) ? 0 : 1;
}
static int run_child_arg(void (*setup)(), int (*fn)(const char*), const char* arg) {
    pid_t pid = fork();
    if (pid == 0) {
        setup();
        int rc = fn(arg);
        std::fflush(stdout);
        _exit(rc == 0 ? 0 : 1);
    }
    int status = 0;
    while (waitpid(pid, &status, 0) < 0) {}
    return (WIFEXITED(status) && WEXITSTATUS(status) == 0) ? 0 : 1;
}

static const char* kEnvFile = nullptr;
static void setup_none()  { unsetenv("AGENTTY_PASSPHRASE"); unsetenv("AGENTTY_ENCRYPT_PASSPHRASE"); }
static void setup_pass()  { setup_none(); setenv("AGENTTY_PASSPHRASE", "correct horse battery staple", 1); setenv("CRED_TEST_OUT", kEnvFile, 1); }
static void setup_wrong() { setup_none(); setenv("AGENTTY_PASSPHRASE", "totally wrong guess", 1); }
static void setup_scrypt(){ setup_pass(); setenv("AGENTTY_KDF", "scrypt", 1); }
#endif

int main() {
#if HAVE_FORK
    // Temp file for the emitted v2 envelope, shared across children.
    char tmpl[] = "/tmp/cred_test_XXXXXX";
    int fd = mkstemp(tmpl);
    if (fd >= 0) close(fd);
    kEnvFile = tmpl;

    int fails = 0;
    fails += run_child(setup_none,   case_v1_machine_only);
    fails += run_child(setup_pass,   case_v2_passphrase);   // writes kEnvFile (argon2id)
    fails += run_child_arg(setup_none,  case_v2_read_without_pass, kEnvFile);
    fails += run_child_arg(setup_wrong, case_v2_read_wrong_pass,   kEnvFile);
    fails += run_child_arg(setup_pass,  case_v2_read_correct_pass, kEnvFile);
    // Forced-scrypt seal + read-back in a fresh child, then reopen it.
    fails += run_child(setup_scrypt,    case_v2_passphrase);   // overwrites kEnvFile (scrypt)
    fails += run_child_arg(setup_pass,  case_v2_read_correct_pass, kEnvFile);

    unlink(kEnvFile);
    if (fails == 0) { std::printf("\nALL CRED-CRYPT CASES PASSED\n"); return 0; }
    std::printf("\n%d CRED-CRYPT CASE(S) FAILED\n", fails);
    return 1;
#else
    // No fork: exercise only the machine-only path in-process.
    int f = case_v1_machine_only();
    if (f == 0) { std::printf("\nMACHINE-ONLY CASE PASSED (fork unavailable)\n"); return 0; }
    return 1;
#endif
}
