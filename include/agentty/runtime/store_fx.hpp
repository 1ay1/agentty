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

#include <jaal/jaal.hpp>

#include "agentty/domain/catalog.hpp"        // ModelInfo
#include "agentty/domain/conversation.hpp"   // Thread
#include "agentty/domain/id.hpp"             // ThreadId
#include "agentty/domain/smart_mode.hpp"     // smart::RoleConfig
#include "agentty/provider/selection.hpp"    // provider::Selection
#include "agentty/store/store.hpp"           // store::Settings

namespace agentty {

/// Persist a thread (create or overwrite).
///
/// Carries the Thread BY VALUE, and that is deliberate: the effect is run
/// after the reducer returns, on a worker, so borrowing from the Model
/// would be a dangling reference the moment the next message lands. The
/// copy is what makes "describe now, run later" safe.
struct SaveThread { Thread thread; };
using save_thread = jaal::pure_fx<SaveThread, "save_thread">;

/// Remove a thread from the store.
struct DeleteThread { ThreadId id; };
using delete_thread = jaal::pure_fx<DeleteThread, "delete_thread">;

/// Write a file to disk. The diff-review pane's accept/reject path.
struct WriteFile { std::string path; std::string contents; };
using write_file = jaal::pure_fx<WriteFile, "write_file">;

/// Persist the settings record.
///
/// Write-behind at the store: this returns immediately and the record
/// reaches disk on a background worker, which is what makes saving on every
/// keystroke affordable. `m.d.persisted` is the record — see save_record in
/// runtime/app/update/internal.hpp for why there is exactly one write path.
struct SaveSettings { store::Settings settings; };
using save_settings = jaal::pure_fx<SaveSettings, "save_settings">;

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
using publish_subagent = jaal::pure_fx<PublishSubagent, "publish_subagent">;

/// Publish the Model's active provider to the process-global copy that code
/// off the loop reads (provider::active(): the stream worker, ACP, main).
///
/// The Model owns the selection. Reducers used to call provider::select()
/// directly, which wrote that global from inside update and left the Model
/// not knowing its own provider. Now only the dispatch seam returns this,
/// and only when the selection changed.
struct PublishSelection { provider::Selection selection; };
using publish_selection = jaal::pure_fx<PublishSelection, "publish_selection">;

}  // namespace agentty
