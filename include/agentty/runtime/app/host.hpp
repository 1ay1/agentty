#pragma once
// agentty::app::Host — maya's terminal host, plus agentty's own effects.
//
// maya::run() builds a terminal_host and hands it to jaal. That host knows
// every TERMINAL effect (scrollback, clipboard, suspend, …) and nothing
// else, which is correct: writing a thread to disk is not a thing a
// terminal does.
//
// So agentty wraps it. Everything maya's host handles is inherited; the
// four persistence effects get a handle() here. jaal's HostFor concept
// checks the union at compile time — an effect in agentty's Cmd row with
// no handler anywhere fails the build and NAMES itself, rather than
// silently never running.
//
// Why a wrapper and not a fork of maya::run: the terminal half is maya's
// business and should keep evolving there. This only adds the half maya
// can't know about.

#include <filesystem>
#include <utility>

#include <maya/host/terminal.hpp>

#include "agentty/auth/accounts.hpp"
#include "agentty/io/persistence.hpp"
#include "agentty/provider/credentials.hpp"
#include "agentty/provider/auth_state.hpp"
#include "agentty/provider/chatgpt/codex_oauth.hpp"   // SaveCodexCredentials
#include "agentty/auth/vault.hpp"
#include "agentty/runtime/app/deps.hpp"
#include "agentty/runtime/app/program.hpp"
#include "agentty/runtime/store_fx.hpp"
#include "agentty/tool/subagent.hpp"            // PublishSubagent's target
#include "agentty/tool/web_search_policy.hpp"       // PublishWebSearchPolicy's target
#include "agentty/tool/util/fs_helpers.hpp"
#include "agentty/util/logx.hpp"

namespace agentty::app {

/// The host agentty actually runs on.
///
/// Inherits maya::terminal_host's handle()/start_source() set and adds the
/// store effects. Public inheritance is what makes the inherited overloads
/// visible to jaal's `handles<H, D, Msg>` probe.
template <class P>
struct Host : maya::terminal_host<P> {
    using maya::terminal_host<P>::terminal_host;

    // Declaring handle() below HIDES every inherited overload — name lookup
    // stops at the first scope that has the name, so jaal's
    // `h.handle(CommitScrollback{...})` probe would fail and the build would
    // report maya's own effects as unrunnable. This puts them back in scope.
    using maya::terminal_host<P>::handle;

    // ── The host_context hooks ──────────────────────────────────────
    //
    // Nothing to write here — maya's attach()/on_ready() are templated on the
    // context type, so they match `host_context<Host>` and are inherited.
    //
    // That was not free. They used to take `host_context<terminal_host<P>>`
    // exactly, and jaal detects the hooks with
    // `requires { host.attach(cx); }` where cx is host_context<THIS host>.
    // A derived host therefore failed the test, the kernel skipped attach(),
    // the terminal's input was never registered with the reactor — and the
    // app came up accepting no keys, silently. Same shape as the init() bug
    // in program.hpp: an optional hook the runtime can't call reads as
    // "absent" rather than as an error. Fixed in maya; see the note there.

    // ── agentty's effects ────────────────────────────────────────────────
    // Each is one call into the persistence layer. They run on the loop
    // thread after the reducer returns; the ones that would block (settings)
    // are write-behind inside persistence itself, so none of these stall a
    // frame.
    //
    // The payloads were copied when the reducer built the effect, so there
    // is nothing here borrowing from a Model that has since moved on.
    //
    // Lock order: jaal calls handle() on the loop thread between steps,
    // where no lock is held (update() and view() can't take locks at all).
    // So each handler runs with `unlocked`, minted here, once per call.
    void handle(SaveThread e) {
        with_unlocked([&](const maya::unlocked& c) { persistence::save_thread(io_, c, e.thread); });
    }
    void handle(DeleteThread e) {
        with_unlocked([&](const maya::unlocked& c) { persistence::delete_thread(io_, c, e.id); });
    }

    // Publish the subagent router's view (see store_fx.hpp PublishSubagent).
    // A mutex-guarded copy into the registry — microseconds — so it runs here
    // on the loop thread, before the next fold. That ordering is the point:
    // a `task` tool dispatched by a LATER message always sees the view the
    // Model had when it was dispatched, never a stale one.
    void handle(PublishSubagent e) {
        namespace sa = tools::subagent;
        sa::set_model(io_, std::move(e.model));
        sa::set_provider(io_, std::move(e.provider));
        sa::set_smart(io_, std::move(e.smart));
        sa::set_candidates(io_, std::move(e.candidates));
    }

    // Publish the Model's active provider (see store_fx.hpp PublishSelection).
    // The one writer of the process-global selection after launch.
    void handle(PublishSelection e) { provider::select(io_, std::move(e.selection)); }

    // Publish the Model's web_search policy (see store_fx.hpp
    // PublishWebSearchPolicy). The one writer of the tool layer's copy once the
    // app is running; one atomic store.
    void handle(PublishWebSearchPolicy e) {
        tools::web_search_policy::install(std::move(e.policy));
    }

    // Credentials (see store_fx.hpp). Small local reads/writes, run in order
    // on the loop thread so the next fold's stream launch sees them.
    // Answers with a fresh AuthView: every credential change ends in an
    // InstallAuth, so the Model's view follows it without each reducer
    // having to remember a LoadAuthView.
    std::optional<Msg> handle(InstallAuth e) {
        update_auth(io_, e.clear ? auth::AuthHeader{}
                            : provider::credentials::resolve(io_, e.provider));
        return handle(LoadAuthView{});
    }
    std::optional<Msg> handle(LoadAuthView) {
        // Through the settings seam, so a key written by an AccountOp in
        // this same batch is seen.
        auto s = deps(io_).load_settings();
        auto view = provider::load_auth_view(io_, s);
        return Msg{msg::LoginMsg{AuthViewLoaded{
            std::move(view), std::move(s.provider_keys)}}};
    }

    void handle(AccountOp e) {
        namespace cr = provider::credentials;
        using K = AccountOp::Kind;
        with_unlocked([&](const maya::unlocked& cap) {
            switch (e.kind) {
                case K::Activate:    (void)cr::activate(io_, cap, e.provider, e.label); break;
                case K::Remove:      (void)cr::remove(io_, cap, e.provider, e.label); break;
                case K::Register:
                    (void)auth::accounts::snapshot_active(io_, cap, e.provider, e.label);
                    break;
                case K::AddKey:      (void)cr::add_key(io_, cap, e.provider, e.key); break;
                case K::SignOut:     auth::vault::sign_out(io_, cap, e.provider); break;
                case K::ClearActive: cr::clear_active(io_, cap, e.provider); break;
            }
        });
    }

    void handle(SaveCredentials e) {
        auth::save_credentials(io_, e.creds);
        namespace acc = auth::accounts;
        const std::string provider = "anthropic";
        // Re-login of the same account reuses its label; "+ Add another"
        // picks the next free one.
        std::string base = acc::derive_current_label(io_, provider);
        if (base.empty()) base = "account";
        std::string label = base;
        if (e.as_new_account)
            for (int n = 2; acc::get(io_, provider, label).has_value() && n < 100; ++n)
                label = base + " " + std::to_string(n);
        with_unlocked([&](const maya::unlocked& cap) {
            acc::snapshot_active(io_, cap, provider, label);
        });
    }

    // The ChatGPT login's credential write (see store_fx.hpp). Loop thread,
    // between steps: `unlocked`. A failure is logged; the provider switch
    // that follows finds no credentials and the next turn reports
    // "not signed in", which is the same state the user would see.
    void handle(SaveCodexCredentials e) {
        with_unlocked([&](const maya::unlocked& cap) {
            if (!provider::chatgpt::save_codex_credentials(cap, e.creds))
                AGT_LOG(Auth, Warn, "codex.save",
                        "signed in, but encrypted credentials could not be saved");
        });
    }

    // Settings go through the write-behind seam, NOT straight to disk.
    //
    // One save is a load-modify-fsync-rename. Effects run on the loop
    // thread, between two frames, and the appearance pane saves on every
    // keystroke — so calling persistence::save_settings here would put a
    // disk round-trip in the input path, which is the exact hitch
    // runtime/app/settings_cache.hpp exists to remove. `deps().save_settings`
    // is that seam (install_deps wraps the store in it, so there is no
    // opt-in to forget): it publishes to memory, returns, and lets one
    // worker do the IO, coalescing a held-down key into a single write.
    //
    // The drain happens once at teardown — the cache registers itself with
    // util::teardown — so a save queued by the Quit arm still reaches disk.
    //
    // This is the one effect that goes through Deps rather than straight to
    // persistence, and it is worth the asymmetry: the alternative is a
    // second write-behind implementation living in the host.
    void handle(SaveSettings e) { deps(io_).save_settings(e.settings); }

    void handle(WriteFile e) {
        // Best-effort, exactly as the Deps seam was: diff-review reject
        // rewrites the file with only the accepted hunks kept. An error is
        // surfaced by the next read (the pane shows the file unchanged), and
        // throwing out of an effect would take the loop down for something
        // the user can recover from.
        (void)tools::util::write_file(std::filesystem::path{e.path},
                                      e.contents);
    }

private:
    // The loop thread between steps holds nothing; see the note above.
    // This file is the effect host's capability root (lint: cap_roots).
    template <class F>
    static decltype(auto) with_unlocked(F&& f) {
        return maya::thread_root(std::forward<F>(f));
    }

    // The host runs effects; that's what an Io is for.
    Io io_ = IoAccess::grant();
};

}  // namespace agentty::app
