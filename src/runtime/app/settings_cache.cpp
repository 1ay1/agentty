// settings_cache.cpp — write-behind for the settings seam. See the header.
//
// Lock order (agentty/util/lock_levels.hpp). One lock, the cache State, a
// LEAF: every body below is a few lines over the State's fields and calls
// nothing that locks. Disk IO happens between bodies, never inside one.
// The drain worker is NOT in a lock (it joins on destruction, so jaal
// refuses it inside a guarded): it lives in a published slot, created by
// the one caller that flips `started` and stopped with `unlocked`.

#include "agentty/runtime/app/settings_cache.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <thread>
#include <utility>

#include "agentty/util/sendable.hpp"   // Id<Tag> inside Settings

#include "agentty/io/persistence.hpp"
#include "agentty/util/background.hpp"   // util::WorkerGroup
#include "agentty/util/lock_levels.hpp"
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

    bool writing    = false;   // a write is in flight right now
    bool stopping   = false;
    // Bumped by invalidate(). A disk load that started before a bump read
    // the old file and must not publish it.
    std::uint64_t gen = 0;
    bool started    = false;   // the drain worker has been posted
    bool registered = false;   // the teardown hook is installed
};

maya::guarded<State>& state() {
    static maya::guarded<State> s;
    return s;
}

// The disk functions, installed by wrap() and read by the worker. Closures
// are not values to pass through a lock, so they are published as one whole.
// The cache's own save goes through save_settings_quiet, so it does NOT
// fire the "settings were written" observer (which would invalidate the
// cache from its own write -- what the old thread-id check guarded against).
struct Disk {
    std::function<store::Settings()>                                 load;
    std::function<void(const maya::unlocked&, const store::Settings&)> save;
};
maya::published<const Disk>& disk() {
    static maya::published<const Disk> d;
    return d;
}

// The drain worker, outside every lock.
maya::published<util::WorkerGroup>& worker() {
    static maya::published<util::WorkerGroup> w;
    return w;
}

void save_now(const maya::unlocked& cap, const Disk& d, const store::Settings& value) noexcept {
    try {
        if (d.save) d.save(cap, value);
    } catch (...) { /* best-effort: a failed save must not kill the app */ }
}

// Drain loop. Runs on its own thread; the ONLY place settings IO happens.
void run(const maya::unlocked& cap) {
    for (;;) {
        // Sleep until there is something to write or we are told to stop.
        // Take the value and mark the write in flight in the same step.
        auto value = state().wait_with(cap,
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
        if (auto d = disk().current()) save_now(cap, *d, *value);

        // flush() waits on this: "queue empty AND nothing in flight".
        state().with(cap, [](State& s) { s.writing = false; });
    }
}

// Called by the one caller that claimed `started`.
void start_worker(const maya::below<maya::leaf>& cap) {
    // THE fix for the abort-on-exit: the subsystem registers its own join the
    // moment it first owns a thread, instead of relying on main().
    const bool reg = state().with(cap, [](State& s) {
        return !std::exchange(s.registered, true);
    });
    if (reg) util::teardown::on_shutdown("settings_cache",
                                         [](const maya::unlocked& c) { shutdown(c); });
    auto g = std::make_shared<util::WorkerGroup>("settings_cache.drain");
    g->post([](const maya::unlocked& job_cap, std::stop_token) { run(job_cap); });
    worker().publish(std::move(g));
}

} // namespace

Seam wrap(const maya::unlocked& cap, std::function<store::Settings()> load_from_disk,
          std::function<void(const maya::unlocked&, const store::Settings&)> save_to_disk) {
    disk().publish(std::make_shared<const Disk>(
        Disk{std::move(load_from_disk), std::move(save_to_disk)}));
    // A re-install (provider switch rebuilds Deps) must not resurrect a stale
    // cache from the previous store.
    state().with(cap, [](State& s) {
        s.cached.reset();
        s.stopping = false;
    });

    // Anyone writing settings.json directly — the credential helpers, the
    // --model/--provider CLI paths — invalidates us, or their write would be
    // undone by the next save publishing a copy that predates it. Our own
    // writes go through save_settings_quiet, so they never reach here.
    persistence::on_settings_written([](const maya::unlocked& c) { invalidate(c); });

    Seam out;

    out.load = [](const maya::below<maya::leaf>& c) {
        auto [hit, gen] = state().read(c, [](const State& s) {
            return std::pair{s.cached, s.gen};
        });
        if (hit) return *hit;

        // First read: fault in from disk with the lock released, then publish.
        store::Settings fresh;
        try {
            if (auto d = disk().current(); d && d->load) fresh = d->load();
        } catch (...) { /* defaults stand */ }
        // Another thread may have seeded (or a save may have published a
        // newer value) meanwhile; theirs wins. If an invalidate ran, our read
        // may predate the write it announced, so return it without caching.
        return state().with(c, [](State& s, store::Settings f, std::uint64_t g) {
            if (s.cached) return *s.cached;
            if (s.gen == g) s.cached = f;
            return f;
        }, std::move(fresh), gen);
    };

    out.save = [](const maya::below<maya::leaf>& c, const store::Settings& value) {
        // Publish to readers FIRST. A reducer that saves then loads must see
        // its own write, and it must see it without waiting for disk.
        const bool start = state().with(c, [](State& s, store::Settings v) {
            s.cached  = v;
            s.pending = std::move(v);
            if (s.started || s.stopping) return false;
            s.started = true;
            return true;
        }, value);
        if (start) start_worker(c);
    };

    return out;
}

void flush(const maya::unlocked& cap) noexcept {
    // No worker ever started, but a value may have been published before one
    // existed: write it here (only reached at exit). Otherwise wait for the
    // queue to empty with nothing in flight.
    auto unstarted = state().with(cap, [](State& s) -> std::optional<store::Settings> {
        if (s.started || !s.pending) return std::nullopt;
        auto v = std::move(*s.pending);
        s.pending.reset();
        return v;
    });
    if (unstarted) {
        if (auto d = disk().current()) save_now(cap, *d, *unstarted);
        return;
    }
    state().wait_with(cap,
        [](const State& s) { return !s.started || (!s.pending && !s.writing); },
        [](State&) {});
}

void invalidate(const maya::unlocked& cap) noexcept {
    // Drain first. A queued write holds a value that predates the bypassing
    // write we are being told about; letting it land afterwards would undo
    // that write on disk, which is the exact failure this exists to stop.
    flush(cap);
    state().with(cap, [](State& s) {
        s.cached.reset();
        ++s.gen;
    });
}

void shutdown(const maya::unlocked& cap) noexcept {
    // Drain BEFORE stopping the worker: a value published moments before quit
    // must still reach disk, otherwise the last thing the user changed is the
    // one thing that does not persist.
    flush(cap);

    const bool was_started = state().with(cap, [](State& s) {
        s.stopping = true;
        const bool was = s.started;
        s.started = false;
        return was;
    });
    if (!was_started) return;
    // run() exits on `stopping` (the with() above woke it); the group joins
    // it, outside every lock.
    if (auto g = worker().take()) g->stop(cap);
}

} // namespace agentty::app::settings_cache
