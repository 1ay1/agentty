// settings_cache.cpp — write-behind for the settings seam. See the header.

#include "agentty/runtime/app/settings_cache.hpp"

#include <memory>
#include <optional>
#include <thread>
#include <utility>

#include "agentty/util/sendable.hpp"   // Id<Tag> inside Settings

#include "agentty/io/persistence.hpp"
#include "agentty/util/background.hpp"   // util::WorkerGroup
#include "agentty/util/teardown.hpp"

namespace agentty::app::settings_cache {

namespace {

// Everything the drain worker and its callers share. Plain data, so it lives
// in a guarded; flush() and the worker wait on it with wait_with.
struct State {
    // The authoritative in-memory value. Once seeded, `load` never touches
    // disk again — which is what gives read-your-writes and also removes a
    // disk read from every reducer that does load-modify-save.
    std::optional<store::Settings> cached;

    // The newest value not yet written. Consecutive saves COALESCE here: the
    // intermediate states of a held-down key are not interesting, only where
    // it stopped. This also bounds the queue at one regardless of how fast
    // the UI produces writes.
    std::optional<store::Settings> pending;

    bool writing  = false;   // a write is in flight right now
    bool stopping = false;
    bool started  = false;   // the drain worker has been posted
    // The thread inside save_now(), so the write observer can tell our own
    // save from a bypassing one.
    std::optional<std::thread::id> saver;
};

maya::guarded<State>& state() {
    static maya::guarded<State> s;
    return s;
}

// The disk functions, installed by wrap() and read by the worker. Closures
// are not values to pass through a lock, so they are published as one whole.
struct Disk {
    std::function<store::Settings()>            load;
    std::function<void(const store::Settings&)> save;
};
maya::published<const Disk>& disk() {
    static maya::published<const Disk> d;
    return d;
}

// The drain worker: a jaal-backed group (one isolated job, joined without a
// deadline on stop) so queued saves always land. Its own lock, taken BEFORE
// state() when both are needed and never inside it, so stop() can wait for
// run() (which only takes state()) without a cycle.
struct Worker {
    std::optional<util::WorkerGroup> group;
    bool registered = false;   // teardown hook installed
};
maya::guarded<Worker>& worker() {
    static maya::guarded<Worker> w;
    return w;
}

// Our save fires the same "settings were written" observer as everyone
// else's. Acting on it would deadlock: invalidate() drains, waiting on
// `writing`, which only this thread clears. So record who is saving.
void save_now(const Disk& d, const store::Settings& value) noexcept {
    state().with([](State& s) { s.saver = std::this_thread::get_id(); });
    try {
        if (d.save) d.save(value);
    } catch (...) { /* best-effort: a failed save must not kill the app */ }
    state().with([](State& s) { s.saver.reset(); });
}

// Drain loop. Runs on its own thread; the ONLY place settings IO happens.
void run() {
    for (;;) {
        // Sleep until there is something to write or we are told to stop.
        // Take the value and mark the write in flight in the same step.
        auto value = state().wait_with(
            [](const State& s) { return s.pending.has_value() || s.stopping; },
            [](State& s) -> std::optional<store::Settings> {
                if (!s.pending) return std::nullopt;   // stopping, queue empty
                auto v = std::move(*s.pending);
                s.pending.reset();
                s.writing = true;
                return v;
            });
        if (!value) return;

        // IO with the lock RELEASED: a reader must never wait on a disk write,
        // which is the entire point of this file.
        if (auto d = disk().current()) save_now(*d, *value);

        // flush() waits on this: "queue empty AND nothing in flight".
        state().with([](State& s) { s.writing = false; });
    }
}

// Called by the one caller that claimed `started`.
void start_worker() {
    worker().with([](Worker& w) {
        // THE fix for the abort-on-exit: the subsystem registers its own join
        // the moment it first owns a thread, instead of relying on main().
        if (!w.registered) {
            w.registered = true;
            util::teardown::on_shutdown("settings_cache", [] { shutdown(); });
        }
        if (!w.group) w.group.emplace("settings_cache.drain");
        w.group->post([](std::stop_token) { run(); });
    });
}

} // namespace

Seam wrap(std::function<store::Settings()> load_from_disk,
          std::function<void(const store::Settings&)> save_to_disk) {
    disk().publish(std::make_shared<const Disk>(
        Disk{std::move(load_from_disk), std::move(save_to_disk)}));
    worker().with([](Worker& w) {
        const bool running = state().with([](State& s) {
            // A re-install (provider switch rebuilds Deps) must not resurrect
            // a stale cache from the previous store.
            s.cached.reset();
            s.stopping = false;
            return s.started;
        });
        // A stopped group is single-use; a re-install after shutdown gets a
        // fresh one on the next save.
        if (!running) w.group.reset();
    });

    // Anyone writing settings.json directly — the credential helpers, the
    // --model/--provider CLI paths — invalidates us, or their write would be
    // undone by the next save publishing a copy that predates it.
    persistence::on_settings_written([] {
        const bool ours = state().read([](const State& s) {
            return s.saver == std::this_thread::get_id();
        });
        if (ours) return;
        invalidate();
    });

    Seam out;

    out.load = [] {
        if (auto hit = state().read([](const State& s) { return s.cached; })) return *hit;

        // First read: fault in from disk with the lock released, then publish.
        store::Settings fresh;
        try {
            if (auto d = disk().current(); d && d->load) fresh = d->load();
        } catch (...) { /* defaults stand */ }
        // Another thread may have seeded (or a save may have published a
        // newer value) meanwhile — theirs wins, being at least as new.
        return state().with([](State& s, store::Settings f) {
            if (!s.cached) s.cached = std::move(f);
            return *s.cached;
        }, std::move(fresh));
    };

    out.save = [](const store::Settings& value) {
        // Publish to readers FIRST. A reducer that saves then loads must see
        // its own write, and it must see it without waiting for disk.
        const bool start = state().with([](State& s, store::Settings v) {
            s.cached  = v;
            s.pending = std::move(v);
            if (s.started || s.stopping) return false;
            s.started = true;
            return true;
        }, value);
        if (start) start_worker();
    };

    return out;
}

void flush() noexcept {
    // No worker ever started, but a value may have been published before one
    // existed: write it here (only reached at exit). Otherwise wait for the
    // queue to empty with nothing in flight.
    auto unstarted = state().with([](State& s) -> std::optional<store::Settings> {
        if (s.started || !s.pending) return std::nullopt;
        auto v = std::move(*s.pending);
        s.pending.reset();
        return v;
    });
    if (unstarted) {
        if (auto d = disk().current()) save_now(*d, *unstarted);
        return;
    }
    state().wait_with(
        [](const State& s) { return !s.started || (!s.pending && !s.writing); },
        [](State&) {});
}

void invalidate() noexcept {
    // Drain first. A queued write holds a value that predates the bypassing
    // write we are being told about; letting it land afterwards would undo
    // that write on disk, which is the exact failure this exists to stop.
    flush();
    state().with([](State& s) { s.cached.reset(); });
}

void shutdown() noexcept {
    // Drain BEFORE stopping the worker: a value published moments before quit
    // must still reach disk, otherwise the last thing the user changed is the
    // one thing that does not persist.
    flush();

    const bool was_started = state().with([](State& s) {
        s.stopping = true;
        const bool was = s.started;
        s.started = false;
        return was;
    });
    if (!was_started) return;
    // run() exits on `stopping` (the with() above woke it); the group joins it.
    worker().with([](Worker& w) { if (w.group) w.group->stop(); });
}

} // namespace agentty::app::settings_cache
