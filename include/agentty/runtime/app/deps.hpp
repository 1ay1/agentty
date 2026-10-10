#pragma once
// agentty::app::Deps — type-erased handle to the runtime's seams.
//
// AgenttyApp's static methods need access to the Provider, Store, and
// credentials that main() wired up. Rather than templating AgenttyApp on three
// type parameters (which forces every translation unit to know the concrete
// types), we use a tiny vtable-style struct that the per-domain update code
// calls into.
//
// The concrete deps are stored once at startup via install_deps().  Anything
// satisfying the relevant concept can be installed; the concrete type stays
// hidden behind std::function-style erasure.

#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "agentty/auth/auth.hpp"
#include "agentty/domain/conversation.hpp"
#include "agentty/runtime/msg.hpp"
#include "agentty/provider/provider.hpp"
#include "agentty/runtime/app/settings_cache.hpp"
#include "agentty/store/store.hpp"
#include "agentty/util/io.hpp"

namespace agentty::app {

struct Deps {
    // ── Provider seam ────────────────────────────────────────────────────
    std::function<void(provider::Request, provider::EventSink)> stream;

    // ── Store seam (just the calls update.cpp actually makes) ────────────
    std::function<void(const Thread&)>          save_thread;
    std::function<void(const ThreadId&)>        delete_thread;
    // Returns thread *metadata* (empty messages) for the picker. Full
    // bodies are fetched on demand via load_thread.
    std::function<std::vector<Thread>()>        load_threads;
    std::function<std::optional<Thread>(const ThreadId&)> load_thread;
    std::function<store::Settings()>            load_settings;
    // WRITE-BEHIND. Returns immediately: the value is published to an
    // in-memory cache that `load_settings` reads, and the actual
    // load-modify-fsync-rename happens on a background worker.
    //
    // Why this is a SEAM concern and not each caller's problem: settings are
    // written from ~8 reducers (profile cycle, Smart Mode toggle, slot clear,
    // provider switch, model select, quit, RAG commit …). A reducer is a pure
    // function on the UI thread; a synchronous disk round-trip inside one
    // stalls the render loop, which is what made toggling Smart Mode hitch
    // mid-animation. Fixing that per call site is eight chances to forget —
    // and every new settings write would be a ninth. Fixing it HERE means no
    // reducer can block on settings IO even if it tries.
    //
    // Ordering: writes are applied in submission order on a single worker, so
    // a later save cannot land before an earlier one. Reads see the newest
    // submitted value immediately, so a save-then-load in the same reducer
    // observes what it just wrote.
    std::function<void(const store::Settings&)> save_settings;
    std::function<ThreadId()>                    new_thread_id;
    std::function<std::string(std::string_view)> title_from;
    // Persist a reviewed file's decided contents to disk. Used by diff-review
    // when the user rejects hunks: the file is rewritten with only the
    // ACCEPTED hunks kept (rejected ones reverted). (path, contents) — a
    // small, user-initiated write, so it runs synchronously in the reducer.
    std::function<void(const std::string&, const std::string&)> write_file;

    // The auth header to start with. install_deps() moves it into the live
    // store below; read it with live_auth(), replace it with update_auth().
    auth::AuthHeader auth;
};

[[nodiscard]] const Deps& deps();
void install_deps(Deps d);

// Resolve the credential for the ACTIVE provider, as a value.
//
// Call this on the UI thread and hand the result to whatever needs it. It
// used to be called FROM worker bodies (the catalog fetch, the window
// probe), which is why it takes the lock at all: a worker reading the auth
// cache races the UI thread's swap mid provider-switch. Those bodies now
// take the header as an argument — jaal's rule that everything a task
// needs is an argument — so the remaining callers are all on the UI
// thread and the lock guards only the login flow's live replace.
//
// It resolves through provider::credentials rather than reading the cache
// directly, so the credential can never drift from the provider it is for —
// the class of bug behind an Anthropic OAuth token being sent to Mistral.
//
// Inside a fold, pass the MODEL's selection (m.d.selection): the process
// global is only published after the fold, so mid-switch it still names the
// provider being left. The no-argument form is for code off the loop, which
// has no Model and reads the published copy.
[[nodiscard]] auth::AuthHeader auth_snapshot(Io, const provider::Selection& sel);
[[nodiscard]] auth::AuthHeader auth_snapshot(Io);

// The live auth header: installed by install_deps(), replaced by the host's
// InstallAuth effect. Safe from any thread (a maya::guarded value); a copy
// comes out, so a stream holds its own header for its whole life.
[[nodiscard]] auth::AuthHeader live_auth();
void update_auth(auth::AuthHeader auth);

// Convenience: bind a Provider + Store satisfying the concepts.
template <provider::Provider P, store::Store S>
void install(P& p, S& s, auth::AuthHeader auth) {
    // Settings IO goes through the write-behind cache. Wrapping HERE — the one
    // place a Deps is built — is what makes "a reducer never blocks on
    // settings IO" a property of the framework rather than a rule eight call
    // sites have to remember. See runtime/app/settings_cache.hpp.
    auto seam = settings_cache::wrap(
        [&s] { return s.load_settings(); },
        [&s](const store::Settings& x) { s.save_settings(x); });

    install_deps(Deps{
        .stream = [&p](provider::Request req, provider::EventSink sink) {
            p.stream(std::move(req), std::move(sink));
        },
        .save_thread     = [&s](const Thread& t) { s.save_thread(t); },
        .load_threads    = [&s] { return s.load_threads(); },
        .load_thread     = [&s](const ThreadId& id) { return s.load_thread(id); },
        .load_settings   = std::move(seam.load),
        .save_settings   = std::move(seam.save),
        .new_thread_id   = [&s] { return s.new_id(); },
        .title_from      = [&s](std::string_view t) { return s.title_from(t); },
        .auth            = std::move(auth),
    });
}

} // namespace agentty::app
