// persistence_proactive_test — the proactive-context marker + confidence
// survive a real save→disk→load round-trip.
//
// Regression lock for the persistence fix in src/io/persistence.cpp:
// proactive_context was previously NOT serialized, so a reloaded thread
// surfaced the raw <retrieved-context> block as a plain user turn. Now the
// flag AND the retrieval confidence (which drives the transcript card's
// bar) round-trip. This test drives the PUBLIC persistence API (save_thread
// + flush_pending_saves + load_thread_file) against an isolated $HOME so it
// touches real disk, not a mock.

#include "agentty/io/persistence.hpp"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>

#include <unistd.h>   // getpid

namespace fs = std::filesystem;
using namespace agentty;

static int g_fails = 0;
static void check(bool ok, const char* what) {
    std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++g_fails;
}

int main() {
    // Isolate the data dir: persistence::data_dir() resolves $HOME/.agentty.
    auto tmp = fs::temp_directory_path()
             / ("agentty_persist_test_" + std::to_string(::getpid()));
    fs::remove_all(tmp);
    fs::create_directories(tmp);
    ::setenv("HOME", tmp.c_str(), 1);
    ::unsetenv("USERPROFILE");   // data_dir() prefers this on Windows; clear it

    Thread t;
    t.id    = persistence::new_id();
    t.title = "round-trip";

    // A normal user turn.
    Message user;
    user.id   = MessageId{"m-user"};
    user.role = Role::User;
    user.text = "how does retry backoff work?";
    t.messages.push_back(user);

    // The proactive context turn — the thing under test.
    Message ctx;
    ctx.id                   = MessageId{"m-ctx"};
    ctx.role                 = Role::User;
    ctx.proactive_context    = true;
    ctx.proactive_confidence = 0.82;
    ctx.text = "<retrieved-context>\n[docs:http/retry.md:12]\n"
               "exponential backoff with full jitter\n</retrieved-context>";
    t.messages.push_back(ctx);

    // Assistant reply (proactive_context must NOT leak onto normal turns).
    Message reply;
    reply.id   = MessageId{"m-reply"};
    reply.role = Role::Assistant;
    reply.text = "It uses exponential backoff with full jitter, capped at 30s.";
    t.messages.push_back(reply);

    // ── Round-trip through real disk ────────────────────────────────────
    persistence::save_thread(t);
    persistence::flush_pending_saves();

    auto path = persistence::threads_dir() / (t.id.value + ".json");
    check(fs::exists(path), "thread file written to disk");

    auto loaded = persistence::load_thread_file(path);
    check(loaded.has_value(), "thread reloaded without error");
    if (!loaded) { std::printf("FAILED\n"); return 1; }

    const auto& msgs = loaded->messages;
    check(msgs.size() == 3, "all three messages survived");
    if (msgs.size() != 3) { std::printf("FAILED\n"); return 1; }

    // The proactive turn kept its identity.
    check(msgs[1].proactive_context, "proactive_context flag survived reload");
    check(msgs[1].proactive_confidence > 0.81
       && msgs[1].proactive_confidence < 0.83,
          "proactive_confidence (0.82) survived reload");

    // Neither the plain user nor the assistant turn picked up the flag,
    // and their confidence stays at the -1 "unknown" sentinel.
    check(!msgs[0].proactive_context && !msgs[2].proactive_context,
          "flag did not leak onto normal turns");
    check(msgs[0].proactive_confidence < 0.0
       && msgs[2].proactive_confidence < 0.0,
          "confidence sentinel (-1) preserved on normal turns");

    fs::remove_all(tmp);

    std::printf("%s\n", g_fails == 0 ? "PASSED" : "FAILED");
    return g_fails == 0 ? 0 : 1;
}
