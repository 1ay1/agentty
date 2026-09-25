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

#include <jaal/jaal.hpp>

#include "agentty/domain/conversation.hpp"   // Thread
#include "agentty/domain/id.hpp"             // ThreadId
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

}  // namespace agentty
