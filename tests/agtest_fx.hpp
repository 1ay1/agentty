#pragma once
// agtest::fx — run and inspect the effects a reducer returned.
//
// A reducer describes its effects instead of performing them:
//
//     Cmd cmd = save_record(m);          // a VALUE saying "save the record"
//
// which is what makes a reducer testable without a store. Two things a test
// wants from that value:
//
//   1. ASSERT on it — "this arm saves exactly once".
//
//        CHECK(agtest::fx::count<save_settings>(cmd) == 1);
//        auto* e = agtest::fx::find<SaveSettings>(cmd);
//        REQUIRE(e); CHECK(e->settings.show_changes_strip);
//
//   2. RUN it — for tests that assert on the store afterwards, which is how
//      most of the suite was written when saving happened inside the
//      reducer. `run(cmd, store)` walks the Cmd and performs the
//      persistence effects against a plain Settings/thread sink, so those
//      tests keep asserting what they always did.
//
// Only the store effects are handled. Terminal effects (clipboard, scrollback)
// are the host's and mean nothing here; tasks and timers are the kernel's, and
// a test that wants those should drive jaal::headless instead.
#ifndef AGENTTY_TESTS_AGTEST_FX_HPP
#define AGENTTY_TESTS_AGTEST_FX_HPP

#include <functional>
#include <vector>

#include "agentty/provider/selection.hpp"
#include "agentty/auth/accounts.hpp"
#include "agentty/auth/vault.hpp"
#include "agentty/provider/credentials.hpp"
#include "agentty/runtime/app/deps.hpp"
#include "agentty/runtime/cmd.hpp"
#include "agentty/runtime/store_fx.hpp"
#include "agentty/store/store.hpp"
#include "agentty/tool/subagent.hpp"

namespace agtest::fx {

// Walk every leaf of a Cmd (batches flattened), in order.
template <class F>
void for_each(const agentty::Cmd& c, F&& f) {
    std::visit([&]<class X>(const X& x) {
        using U = std::remove_cvref_t<X>;
        if constexpr (std::same_as<U, typename agentty::Cmd::Batch>) {
            for (const auto& inner : x.cmds) for_each(inner, f);
        } else if constexpr (std::same_as<U, typename agentty::Cmd::None>) {
            // nothing
        } else {
            f(x);
        }
    }, c.inner);
}

/// The first payload of type `P` in the Cmd, or nullptr.
template <class P>
[[nodiscard]] const P* find(const agentty::Cmd& c) {
    const P* hit = nullptr;
    for_each(c, [&](const auto& e) {
        if constexpr (std::same_as<std::remove_cvref_t<decltype(e)>, P>)
            if (!hit) hit = &e;
    });
    return hit;
}

/// How many payloads of type `P` the Cmd carries.
template <class P>
[[nodiscard]] int count(const agentty::Cmd& c) {
    int n = 0;
    for_each(c, [&](const auto& e) {
        if constexpr (std::same_as<std::remove_cvref_t<decltype(e)>, P>) ++n;
    });
    return n;
}

/// A stand-in for the store, so a test can assert on "what reached disk".
struct Store {
    agentty::store::Settings          settings;
    std::vector<agentty::Thread>      saved_threads;
    std::vector<agentty::ThreadId>    deleted_threads;
    std::vector<std::pair<std::string, std::string>> written_files;
};

/// Perform the persistence effects in `c` against `s`, plus the publish
/// effects (tests read the globals the way off-loop code does).
///
/// This is what the HOST does in production (runtime/app/host.hpp); doing it
/// here keeps a test that asserts on the store honest about the fact that a
/// save only happens if the reducer actually returned the effect.
inline void run(const agentty::Cmd& c, Store& s) {
    for_each(c, [&](const auto& e) {
        using U = std::remove_cvref_t<decltype(e)>;
        if constexpr (std::same_as<U, agentty::SaveSettings>)
            s.settings = e.settings;
        else if constexpr (std::same_as<U, agentty::SaveThread>)
            s.saved_threads.push_back(e.thread);
        else if constexpr (std::same_as<U, agentty::DeleteThread>)
            s.deleted_threads.push_back(e.id);
        else if constexpr (std::same_as<U, agentty::WriteFile>)
            s.written_files.emplace_back(e.path, e.contents);
        else if constexpr (std::same_as<U, agentty::PublishSelection>)
            agentty::provider::select(e.selection);
        else if constexpr (std::same_as<U, agentty::InstallAuth>) {
            // Only the clear is played here: resolving would read the
            // developer's real credential files.
            if (e.clear) agentty::app::update_auth(agentty::auth::AuthHeader{});
        }
        else if constexpr (std::same_as<U, agentty::PublishSubagent>) {
            namespace sa = agentty::tools::subagent;
            sa::set_model(e.model);
            sa::set_provider(e.provider);
            sa::set_smart(e.smart);
            sa::set_candidates(e.candidates);
        }
    });
}

/// Run the credential effects (AccountOp, SaveCredentials) against the real
/// credential layer, the way the host does. For tests that point
/// AGENTTY_HOME at a temp dir and assert on the stores afterwards.
inline void run_credentials(const agentty::Cmd& c) {
    namespace cr  = agentty::provider::credentials;
    namespace acc = agentty::auth::accounts;
    for_each(c, [&](const auto& e) {
        using U = std::remove_cvref_t<decltype(e)>;
        if constexpr (std::same_as<U, agentty::AccountOp>) {
            using K = agentty::AccountOp::Kind;
            switch (e.kind) {
                case K::Activate:    (void)cr::activate(e.provider, e.label); break;
                case K::Remove:      (void)cr::remove(e.provider, e.label); break;
                case K::Register:    (void)acc::snapshot_active(e.provider, e.label); break;
                case K::AddKey:      (void)cr::add_key(e.provider, e.key); break;
                case K::SignOut:     agentty::auth::vault::sign_out(e.provider); break;
                case K::ClearActive: cr::clear_active(e.provider); break;
            }
        } else if constexpr (std::same_as<U, agentty::SaveCredentials>) {
            agentty::auth::save_credentials(e.creds);
        }
    });
}

}  // namespace agtest::fx

#endif  // AGENTTY_TESTS_AGTEST_FX_HPP
