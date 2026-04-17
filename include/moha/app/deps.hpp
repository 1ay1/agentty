#pragma once
// moha::app::Deps — type-erased handle to the runtime's seams.
//
// MohaApp's static methods need access to the Provider, Store, and credentials
// that main() wired up.  Rather than templating MohaApp on three type
// parameters (which forces every translation unit to know the concrete types),
// we use a tiny vtable-style struct that the per-domain update code calls into.
//
// The concrete deps are stored once at startup via install_deps().  Anything
// satisfying the relevant concept can be installed; the concrete type stays
// hidden behind std::function-style erasure.

#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "moha/io/provider.hpp"
#include "moha/io/store.hpp"
#include "moha/model.hpp"
#include "moha/msg.hpp"

namespace moha::app {

struct Deps {
    // ── Provider seam ────────────────────────────────────────────────────
    std::function<void(io::ProviderRequest, io::EventSink)> stream;

    // ── Store seam (just the calls update.cpp actually makes) ────────────
    std::function<void(const Thread&)>          save_thread;
    std::function<std::vector<Thread>()>        load_threads;
    std::function<persistence::Settings()>      load_settings;
    std::function<void(const persistence::Settings&)> save_settings;
    std::function<ThreadId()>                    new_thread_id;
    std::function<std::string(std::string_view)> title_from;

    // ── Auth context (immutable for the session) ─────────────────────────
    std::string auth_header;
    auth::Style auth_style = auth::Style::ApiKey;
};

[[nodiscard]] const Deps& deps();
void install_deps(Deps d);

// Convenience: bind a Provider + Store satisfying the concepts.
template <io::Provider P, io::Store S>
void install(P& provider, S& store, std::string auth_header, auth::Style style) {
    install_deps(Deps{
        .stream = [&provider](io::ProviderRequest req, io::EventSink sink) {
            provider.stream(std::move(req), std::move(sink));
        },
        .save_thread     = [&store](const Thread& t) { store.save_thread(t); },
        .load_threads    = [&store] { return store.load_threads(); },
        .load_settings   = [&store] { return store.load_settings(); },
        .save_settings   = [&store](const persistence::Settings& s) { store.save_settings(s); },
        .new_thread_id   = [&store] { return store.new_id(); },
        .title_from      = [&store](std::string_view t) { return store.title_from(t); },
        .auth_header     = std::move(auth_header),
        .auth_style      = style,
    });
}

} // namespace moha::app
