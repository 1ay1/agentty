// SPDX-License-Identifier: Apache-2.0
//
// persistence_race_stubs.cpp — link-time stubs for the settings/vault half
// of persistence.cpp, so persistence_race_test can be built narrow.
//
// WHY. persistence_race_test drives the async THREAD save queue:
// save_thread, load_all_threads, flush_pending_saves. It never calls
// load_settings or save_settings and never touches provider keys.
//
// But persistence.cpp is one translation unit holding both halves, and its
// settings half calls auth::keys::{load,save} -> vault -> keystore ->
// credentials -> the provider registry. Linking that chain pulls ~74 TUs
// for code this test does not execute. Under -fsanitize=thread, where the
// CI race lane lives, every one of those is compiled instrumented.
//
// The alternative to these stubs is what CI did before: build the whole
// 375-TU agentty_standalone_tests under TSan to run this one test.
//
// SAFETY. Each stub stands in for a function the test provably never
// reaches, and each aborts rather than returning a fake value -- so if a
// future edit to persistence.cpp DOES start calling one from the thread
// path, this test dies loudly at that call instead of quietly exercising a
// lie. That is the property that makes stubbing honest here: the stubs
// cannot silently change what the test measures.

#include "agentty/auth/keys.hpp"

#include <cstdio>
#include <cstdlib>

namespace {
[[noreturn]] void unreachable(const char* who) {
    std::fprintf(stderr,
        "persistence_race_test: %s was called, but this narrow build stubs "
        "it out because the THREAD save path is not supposed to reach the "
        "settings/vault half of persistence.cpp.\n"
        "Either the test grew a settings dependency (then link the real "
        "vault sources in cmake/AgenttyTests.cmake), or persistence.cpp's "
        "thread path grew one (then that is the bug).\n", who);
    std::abort();
}
}  // namespace

namespace agentty::auth::keys {

KeyMap load() { unreachable("auth::keys::load"); }
bool save(const KeyMap&) { unreachable("auth::keys::save"); }

}  // namespace agentty::auth::keys

// persistence_race_test.cpp is ALSO a member of the standalone fold, and
// the fold renames each member's main() to <name>_main via a source
// property. That property is set at DIRECTORY scope, so it applies to this
// narrow target's copy of the same source too -- the TU compiles, but no
// `main` symbol comes out of it.
//
// Rather than fight the rename (a second copy of the source, or a
// target-scoped property that would drift from the fold's), just call what
// the rename produced. One line, and it keeps ONE copy of the test source
// feeding both the folded entry and the narrow one, which is the point --
// they can never test different things.
extern int persistence_race_test_main();
int main() { return persistence_race_test_main(); }
