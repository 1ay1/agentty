#pragma once
// agentty::store_fx — persistence as effects, not as calls through a seam.
//
// A reducer used to reach the disk through `Deps`:
//
//     deps().save_thread(m.d.current);      // happens DURING the reducer
//
// which makes the reducer impure: to test that a turn is saved you have to
// install a fake store and inspect it afterwards, and the reducer can no
// longer be read as "state in, state + description-of-effects out".
//
// As effects the same thing is a VALUE the reducer returns:
//
//     return save_thread_fx(m.d.current);   // DESCRIBES a save
//
// The host carries it out (see runtime/app/host.hpp), and a test asserts on
// the returned Cmd — `expect_effect<save_thread>(1)` — with no store at all.
// That is the whole point of the jaal shape; see docs/design/jaal-rewrite.md.
//
// WHY THESE FOUR AND NOT THE WHOLE OF Deps. An effect is something with a
// side effect on the world: writing a thread, deleting one, writing a file,
// writing settings. `Deps` also carried `new_thread_id` and `title_from`,
// which are pure functions that never needed erasing — they're called
// directly now. And `load_*` are READS, which an effect can't express
// (jaal effects don't return values into the reducer); the ones that were
// really "read state the Model already holds" now read `m.d.persisted`, and
// the two genuine reads that remain are documented where they are.

#include <string>
#include <vector>

#include <maya/runtime.hpp>

#include "agentty/domain/catalog.hpp"        // ModelInfo
#include "agentty/domain/conversation.hpp"   // Thread
#include "agentty/domain/thread_delta.hpp"   // ThreadDelta
#include "agentty/domain/id.hpp"             // ThreadId
#include "agentty/domain/smart_mode.hpp"     // smart::RoleConfig
#include "agentty/provider/selection.hpp"    // provider::Selection
#include "agentty/store/store.hpp"           // store::Settings
#include "agentty/auth/auth.hpp"             // auth::Credentials
#include "agentty/provider/chatgpt/codex_oauth.hpp"   // CodexCredentials

namespace agentty {

/// Persist a thread: the part that may have changed since the last save
/// this program described (persistence::describe_save). Owned, so the host
/// can run it after the Model has moved on. A refused delta comes back as
/// SaveRefused and the next save is sent whole.
struct SaveThread { ThreadDelta delta; };
using save_thread = maya::pure_fx<SaveThread, "save_thread">;

/// Remove a thread from the store.
struct DeleteThread { ThreadId id; };
using delete_thread = maya::pure_fx<DeleteThread, "delete_thread">;

/// Store a new title for a saved thread; its messages and timestamps are
/// left alone. `title` is already cleaned (persistence::clean_title).
/// `renamed_at` is the reducer's clock, unix seconds. `name_is_auto` marks
/// a cleared rename (back to the automatic name).
struct RenameThread { ThreadId id; std::string title; std::int64_t renamed_at = 0;
                      bool name_is_auto = false; };
using rename_thread = maya::pure_fx<RenameThread, "rename_thread">;

/// Write a file to disk. The diff-review pane's accept/reject path.
struct WriteFile { std::string path; std::string contents; };
using write_file = maya::pure_fx<WriteFile, "write_file">;

/// Persist the settings record.
///
/// Write-behind at the store: this returns immediately and the record
/// reaches disk on a background worker, which is what makes saving on every
/// keystroke affordable. `m.d.persisted` is the record — see save_record in
/// runtime/app/update/internal.hpp for why there is exactly one write path.
struct SaveSettings { store::Settings settings; };
using save_settings = maya::pure_fx<SaveSettings, "save_settings">;

/// Publish the subagent router's view of the Model to the registry the
/// worker threads read (tool/subagent.cpp).
///
/// The registry is a mirror of four Model fields. Reducers used to write it
/// directly with tools::subagent::set_*(), which made them impure and made
/// keeping it in sync something every reducer had to remember. Now the
/// dispatch seam returns this effect, and only when the view changed, so the
/// host runs it once per change. Carried by value for the same reason as
/// SaveThread: it runs after the reducer returns.
struct PublishSubagent {
    std::string             model;
    std::string             provider;
    smart::RoleConfig       smart;
    std::vector<ModelInfo>  candidates;
};
using publish_subagent = maya::pure_fx<PublishSubagent, "publish_subagent">;

/// Publish the Model's active provider to the process-global copy that code
/// off the loop reads (provider::active(): the stream worker, ACP, main).
///
/// The Model owns the selection. Reducers used to call provider::select()
/// directly, which wrote that global from inside update and left the Model
/// not knowing its own provider. Now only the dispatch seam returns this,
/// and only when the selection changed.
struct PublishSelection { provider::Selection selection; };
using publish_selection = maya::pure_fx<PublishSelection, "publish_selection">;

/// Publish the Model's web_search policy (Settings → Web Search) to the
/// process-wide copy the tool layer reads off the loop: the request builder
/// that decides whether web_search goes on the wire, and the dispatch closure
/// that clamps counts and appends exclusions (tools::web_search_policy).
///
/// Same rule as PublishSelection: the Model owns the value, only the dispatch
/// seam returns this, and only when it changed — including on the very first
/// fold, which is what makes the tool layer agree with the Model from startup
/// on instead of with whatever its own lazy disk read happened to see.
struct PublishWebSearchPolicy { web_search_cfg::Config policy; };
using publish_web_search_policy =
    maya::pure_fx<PublishWebSearchPolicy, "publish_web_search_policy">;

/// Install the Model's paste settings in the clipboard reader (richpaste).
/// Same rule as PublishWebSearchPolicy: only the dispatch seam returns it, on
/// the first fold and whenever m.d.persisted.paste changes.
struct PublishPasteConfig { paste_cfg::Config config; };
using publish_paste_config =
    maya::pure_fx<PublishPasteConfig, "publish_paste_config">;

/// Re-install the live auth header the stream and subagents use.
///
/// Reducers used to resolve the credential themselves (a read of the
/// credential file or settings) and push the result into Deps. Now they say
/// which provider, and the host resolves and installs it. `clear` installs an
/// empty header instead (sign-out, a removed account).
struct InstallAuth {
    std::string provider;
    bool        clear = false;
};
using install_auth = maya::pure_fx<InstallAuth, "install_auth">;

/// Persist Anthropic credentials from a login and file them as an account.
/// `as_new_account` picks a fresh label rather than reusing the current one.
/// Followed by an InstallAuth for "anthropic" so the new header goes live.
struct SaveCredentials {
    auth::Credentials creds;
    bool              as_new_account = false;
};
using save_credentials = maya::pure_fx<SaveCredentials, "save_credentials">;

/// Persist ChatGPT (Codex) OAuth credentials from a finished login. The
/// reducer still decides (only after matching the active attempt id, so an
/// abandoned worker can't overwrite the store); the host performs the write,
/// with `unlocked`, because it is encrypted file IO under the codex creds
/// lock and a reducer can neither block nor take locks. A failed write
/// answers CodexCredentialsSaveFailed.
struct SaveCodexCredentials {
    provider::chatgpt::CodexCredentials creds;
};
using save_codex_credentials = maya::pure_fx<SaveCodexCredentials, "save_codex_credentials">;

/// Read the credential stores, env and accounts registry into an AuthView
/// and answer AuthViewLoaded. Batched after any effect that changes them.
struct LoadAuthView {};
using load_auth_view = maya::pure_fx<LoadAuthView, "load_auth_view">;

/// The accounts registry, as effects. Each runs on the host, then the reducer
/// batches a LoadAuthView so the Model sees the result.
///   Activate  — copy the named slot into the provider's live store.
///   Remove    — drop the slot (the registry promotes the next one).
///   Register  — snapshot the live credential under `label`.
///   AddKey    — save a pasted key as the active account (key providers).
///   SignOut   — clear the provider's live store (vault::sign_out).
///   ClearActive — clear the live store without touching the registry.
struct AccountOp {
    enum class Kind : unsigned char {
        Activate, Remove, Register, AddKey, SignOut, ClearActive,
    };
    Kind        kind = Kind::Activate;
    std::string provider;
    std::string label;   // Activate / Remove / Register
    std::string key;     // AddKey
};
using account_op = maya::pure_fx<AccountOp, "account_op">;

}  // namespace agentty
