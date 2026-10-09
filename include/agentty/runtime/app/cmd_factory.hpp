#pragma once
// agentty::app::cmd — factories for the side-effecting commands the runtime issues.
//
// These wrap maya's Cmd with agentty-specific glue: kicking off a streaming
// turn, executing a tool, advancing pending tool execution after a turn ends.

#include <maya/maya.hpp>

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "agentty/runtime/model.hpp"
#include "agentty/runtime/cmd.hpp"
#include "agentty/runtime/msg.hpp"
#include "agentty/io/http.hpp"   // http::CancelTokenPtr (run_tool)
#include "agentty/provider/selection.hpp"  // provider::Selection (fetch_models)
#include "agentty/tool/plugin.hpp"         // plugin::ServerSpec (edit_plugin)

namespace agentty::app::cmd {

// ── The per-turn routing DECISION ────────────────────────────────
//
// Which model serves this turn, at what effort, and why. ONE value, computed
// once by resolve_turn_routing() — the 🧠 card renders it and launch_stream
// dispatches it.
//
// The two used to derive it INDEPENDENTLY from the same Model, each running
// the same scan, the same classifier and the same effort scaler. The comment
// on both said "same call as launch_stream so card == wire", and keeping that
// true meant passing every argument identically at two sites forever. It did
// not stay true: a threshold added to the classifier reached one site and not
// the other, so the card advertised a route the request never took, and the
// only thing that noticed was a test written specifically to look for it.
//
// With one value there is nothing to keep in step. card == wire stops being an
// invariant under test and becomes an identity.
struct TurnRouting {
    // Empty model ⇒ orchestration is off and the plain selection serves the
    // turn; the card is not shown at all.
    bool                    orchestrate = false;
    std::string             model;      // resolved Strategic model ("" = selection)
    Effort                  base = Effort::None;     // BEFORE complexity scaling
    Effort                  effort = Effort::None;   // AFTER complexity scaling
    smart::ComplexityScore  cx{};       // the classification that scaled it
    smart::ComplexityScore  cx_text{};  // text-only score, for lift provenance
    bool                    subagents = false;

    // The role this turn resolved to. Smart Mode used to pin the main turn to
    // Strategic unconditionally, so the flagship served every turn including
    // the ones the classifier had already scored Trivial — Implementation and
    // Utility were reachable only via a `task` subagent, which many sessions
    // never spawn. The role now follows the complexity tier (clamped by
    // RoleConfig::main_turn_floor), and `model`/`effort` above are that
    // role's profile. Recorded so the card, the log line and the turn header
    // all name the same role the wire actually used.
    smart::ModelRole        role = smart::ModelRole::Strategic;

    // The prompt the decision was made about. Held so the card can NAME why a
    // tier was lifted (payload / continuation / correction) without re-walking
    // the transcript and risking a scan that drifts from the one above.
    std::string             prompt;
};

// THE routing decision for the turn about to launch. Pure: reads the
// transcript, the catalog and m.d.smart, and writes nothing.
//
// Safe to call at submit and read at launch even on the DEFERRED proactive
// path, where retrieval splices a context message in between: the newest-user
// scan skips proactive-context, 🧠-card and fork-note messages, which is
// everything that can land in that window. smart_routing_card_test pins it.
[[nodiscard]] TurnRouting resolve_turn_routing(const Model& m);

// Mutates `m` to install a fresh cancel token in m.stream.cancel, then
// dispatches the streaming task on a worker. Esc (CancelStream) flips the
// token to abort the in-flight stream.
[[nodiscard]] Cmd launch_stream(Model& m);

// Build the Smart Mode ROUTING CARD for the turn about to launch, or
// std::nullopt when orchestration is off (nothing to show). A synthetic,
// wire-inert Message (smart_routing=true) the caller inserts into the
// transcript just before the assistant placeholder so the user sees, as a
// first-class thread event, which model + effort the turn was routed to, the
// classified complexity that scaled it, and which layers are active. Pure;
// mints a fresh MessageId. Renders `routing` — the SAME value launch_stream
// dispatches.
[[nodiscard]] std::optional<Message> build_smart_routing_card(const Model& m);

// What the model actually sees on the next request: applies any
// Thread::CompactionRecord substitution (latest record's summary
// replaces messages[0..up_to_index) on the wire). Mirrors what
// launch_stream's normal-turn branch ships. Callers use this when
// they need to reason about the wire payload size or shape — the
// auto-compaction triggers in particular need to estimate the
// COMPACTED prefix, not the raw transcript, otherwise they re-fire
// immediately after every compaction.
[[nodiscard]] std::vector<Message> wire_messages_for(const Thread& t);

// Wire-only trim to fit `ceiling` estimated tokens. Keeps the head, the
// latest User message and the newest message; drops the rest oldest-first,
// and the head only as a last resort. The result always opens with a User.
void soft_trim_to_ceiling(std::vector<Message>& v, int ceiling);

// Bytes-based prefix token estimate computed against the wire view
// (i.e. with compaction substitution applied). Same approximation as
// `estimate_prefix_tokens(Thread)` but the right denominator for
// auto-compaction logic and the context-gauge.
[[nodiscard]] int estimate_wire_tokens(const Thread& t);

// Fresh, process-unique tag for one tool dispatch. Stamp it on
// ToolUse::Running and pass the same value to run_tool.
[[nodiscard]] std::uint64_t next_tool_exec_seq() noexcept;

[[nodiscard]] Cmd run_tool(ToolCallId id,
                                      ToolName tool_name,
                                      nlohmann::json args,
                                      http::CancelTokenPtr cancel = {},
                                      std::uint64_t exec_seq = 0,
                                      // Fingerprint of the definition consent
                                      // was given for; re-checked immediately
                                      // before execute. 0 skips the check.
                                      std::uint64_t approved_def_hash = 0,
                                      // Skills whose bodies the model can
                                      // already see, from
                                      // skills::active_in(visible_text(thread)).
                                      // The `skill` tool reads it to dedupe.
                                      std::vector<std::string> active_skills = {});

// Inspect the latest assistant turn and either fire off pending tool calls,
// request permission, or kick the follow-up stream once tool results are in.
// Mutates `m` (sets phase, may push a placeholder assistant message).
[[nodiscard]] Cmd kick_pending_tools(Model& m);

// Resolve every PENDING *salvaged* tool call in the back assistant message
// that byte-duplicates a call already terminal earlier in the same agent
// turn, marking it Failed-without-side-effects instead of letting it run a
// second time. Salvaged calls (synthetic `call_salvaged_` ids minted by the
// OpenAI-compat transport when a weak local model leaks a tool call into the
// `content` channel) are the only ones deduped — structured calls are the
// model's deliberate intent. Returns the number deduped. Called by
// kick_pending_tools before any promotion to Running; exposed for tests.
std::size_t dedup_releaked_salvage_calls(Model& m);

// ── Path-aware parallel tool scheduling (pure; exposed for tests) ────────────
// Given a batch of tool calls (the model's emission for one turn) and the set
// already RUNNING, decide which currently-pending calls may be promoted to run
// CONCURRENTLY this tick. The decision refines the coarse effect-only rule
// (is_parallel_safe) with PATH analysis: two writers to disjoint files — or a
// read of a.c alongside a write of b.c — run in parallel instead of
// serialising. Conflicts (overlapping paths, any Exec, or a writer whose path
// can't be extracted) stay pending and advance on the next kick once the
// blocker settles. Submission order is preserved: a call never jumps ahead of
// an earlier conflicting call. `running` and `pending` index into the same
// logical batch; returns the subset of `pending` indices safe to start now.
struct SchedDecision {
    std::vector<std::size_t> promote;   // pending indices to start this tick
};
[[nodiscard]] SchedDecision schedule_parallel_batch(
    const std::vector<ToolUse>& batch);

// ── Doom-loop circuit breaker (pure; exposed for tests) ──────────────────────
// Weak local models (qwen2.5-coder, codellama, …) routinely fall into a
// non-converging tool loop: they pick the wrong tool for a goal (e.g. `read`
// on a URL or a file that doesn't exist), get an error result, and re-issue a
// near-identical call indefinitely. With no native completion signal the main
// agent loop would spin until the user hits Esc — the symptom behind the
// "tool usage is fucked" reports. Mirrors the iteration / repeated-failure
// caps every serious local-agent framework ships (Qwen-Agent, aider, cline).
//
// Given the full message history of the CURRENT agent turn (the run since the
// last real User message), returns a non-empty nudge string when the loop
// should be force-stopped, or std::nullopt to keep going. Two triggers, both
// keyed on REPETITION — the same call with byte-identical arguments — because
// that is what distinguishes a stuck model from a busy one:
//   (1) REPEAT: the same (tool, args) call appears >= kRepeatLimit times and
//       its results were failures — the model is stuck re-trying a dead call.
//       ALWAYS enforced (every production tool does this: aider's
//       max_reflections, MindStudio's "2–3 attempts then stop"). A capable
//       model that genuinely loops on a dead call benefits from it too.
//   (2) SUCCESS-REPEAT: the same (tool, args) call SUCCEEDS >= 6 times in one
//       run — a model re-reading the same path with identical arguments six
//       times and still not speaking is not going to converge on the seventh.
//       Also always enforced.
//
// There is deliberately NO step cap. A raw count of tool turns is not evidence
// of anything: a legitimate search → read → edit → verify run across a large
// codebase spends dozens of distinct, productive steps. Claude Code's
// max_turns is unlimited by default and aider never step-caps, for the same
// reason. The old one was additionally gated on a model-id heuristic, so the
// identical transcript stopped on a local model and ran fine on Claude.
// The returned text is surfaced to the model as the final assistant turn so
// it can recover gracefully (and the user sees why the loop stopped).
struct LoopBreak {
    std::string reason;     // user/model-facing explanation
};
[[nodiscard]] std::optional<LoopBreak> agent_loop_should_break(
    const std::vector<Message>& messages);

// Fetch the model catalog.
//
// The three-argument form takes what the worker needs INSTEAD of reaching
// for it: the body used to call provider::active(), auth_snapshot() and
// active_provider_id() from a worker thread, racing the UI thread's
// provider switch — which is why auth_snapshot() needs a mutex at all.
// jaal's rule for a task body is that everything it needs is an argument
// (docs/concurrency.md §4.6), and a reducer on the UI thread simply knows
// these values.
//
// `for_provider` is the staleness stamp the reducer compares at DELIVERY
// time, so a catalog that lands after a switch is dropped instead of
// installed against the wrong provider.
//
// Declared with the Selection itself: provider::active() already returns a
// by-value snapshot taken under the selection mutex, so handing one to a
// worker is exactly what it is for.
[[nodiscard]] Cmd fetch_models(provider::Selection sel,
                                          auth::AuthHeader   auth,
                                          std::string        for_provider);

// The common case: fetch for the MODEL's active provider. Resolves the
// arguments on the calling (UI) thread from `m` and forwards. Call this from a
// reducer; the overload above is for a caller that already has them.
[[nodiscard]] Cmd fetch_models(const Model& m);

// Fetch a SPECIFIC provider's catalog without switching to it (fused picker
// fan-out). Dispatches FusedCatalogLoaded{spec, models, ok}.
[[nodiscard]] Cmd fetch_models_for(std::string spec);

// Re-measure the live context window of `model_id` on the active provider
// when it is a local OpenAI-compatible endpoint (llama.cpp router, LM Studio,
// Ollama). Dispatches ModelWindowProbed. A no-op Cmd for hosted providers.
[[nodiscard]] Cmd probe_model_window(const Model& m, std::string model_id);

// ── Self-update ─────────────────────────────────────────────
// Background release check (24h-cached, never blocks a frame): dispatches
// UpdateCheckDone. check_for_update() is safe to fire on every launch.
[[nodiscard]] Cmd check_for_update();
// Download + atomically install the given version; dispatches UpdateApplied.
[[nodiscard]] Cmd perform_self_update(std::string version);

// ── In-app login modal ──────────────────────────────────────────────────
// Fire-and-forget: shells out to the platform browser opener. Wrapped in
// Cmd::task so a wedged xdg-open / open / ShellExecute can never block
// the reducer tick.
[[nodiscard]] Cmd open_browser_async(std::string url);

// Mint a PKCE verifier + state and the authorize URL for an Anthropic OAuth
// login; replies with LoginOAuthMinted.
[[nodiscard]] Cmd mint_oauth_login();

// Run the OAuth code-exchange HTTP POST off the UI thread. Dispatches
// LoginExchanged{result} on completion regardless of success/failure —
// the reducer matches on `expected<OAuthToken, OAuthError>` to decide
// whether to install creds or transition to Failed.
[[nodiscard]] Cmd oauth_exchange(auth::OAuthCode    code,
                                            auth::PkceVerifier verifier,
                                            auth::OAuthState   state);

// Run the OAuth refresh HTTP POST off the UI thread. Dispatched from
// `AgenttyApp::init()` when `auth::take_pending_refresh()` returned a
// stashed token (i.e. on-disk creds were expired but had a refresh
// token). The TUI is already drawn by the time this runs, so the user
// sees a sticky "refreshing OAuth token…" toast in the bottom row
// instead of the old pre-TUI stderr line, and startup is no longer
// gated on the network round trip.
[[nodiscard]] Cmd refresh_oauth(std::string refresh_token);

// Read the Anthropic OAuth token and refresh it if it lapses within ~5 min.
// Replies TokenRefreshed when it refreshed, else OAuthRefreshNotDue. The
// caller sets m.s.oauth_refresh_in_flight; both replies clear it.
[[nodiscard]] Cmd refresh_oauth_if_due();

// A 401 parked the stream. Read the stored Anthropic refresh token and
// refresh with it. Replies TokenRefreshed either way: success resumes the
// stream, and "no refresh token stored" fails it with a sign-in hint.
[[nodiscard]] Cmd refresh_oauth_for_401();

// Allocate a process-unique identity for a ChatGPT login attempt. Async
// progress/completion must carry it so an abandoned attempt cannot mutate a
// newer login modal.
[[nodiscard]] std::uint64_t next_codex_login_attempt_id() noexcept;

// Connect-probe a custom host off the UI thread: dial its model list
// (configured path → /v1/models → Ollama /api/tags), detect the dialect,
// dispatch HostProbed. Shares next_codex_login_attempt_id() so a stale
// probe result (user Esc'd / resubmitted) is dropped by the reducer. The
// worker resolves the host's saved key itself.
[[nodiscard]] Cmd probe_host_async(std::string spec, std::uint64_t attempt_id);

// The login workers are SUBSCRIPTIONS, not Cmds.
//
// Both block-poll a provider for up to 900 s while the user signs in, and
// both must stop the moment the user presses Esc. That is precisely what a
// keyed source is for: subscribe() returns one while the modal is up, and
// jaal fires the body's stop_token when the key stops being returned — so
// closing the modal IS cancelling the worker, rather than a second thing
// that has to be remembered. (It was not remembered: as Cmds they polled on
// after Esc, one leaked thread per abandoned attempt.)
//
// jaal also drops messages from a stopped generation, so a late result can
// never land in a model that moved on. attempt_id stays anyway — it is what
// the reducers match on, and it costs nothing.
[[nodiscard]] Sub codex_login_sub(std::uint64_t attempt_id);

// Provider-generic device flow (GitHub Copilot, Kimi, …). `provider` is the
// registry id, `provider_label` the display name for the success toast.
// Dispatches DeviceCodeReady then DeviceLoginDone.
[[nodiscard]] Sub device_login_sub(
    std::string provider, std::string provider_label, std::uint64_t attempt_id);

// Walk ~/.agentty/threads/ and parse every thread JSON off the UI thread.
// Dispatches `ThreadsLoaded{vec}` on completion. The directory walk +
// parse can take seconds with hundreds of multi-MB files in real-world
// use, so it runs as a background task instead of blocking startup;
// `init()` returns immediately with an empty thread list.
[[nodiscard]] Cmd load_threads_async();

// Connect/snapshot the MCP servers off the UI thread and dispatch
// `PluginsUpdated{model}` when done — the reducer stores it in m.ui.plugins,
// the single source the Plugins panel renders. reconnect=true respawns +
// re-handshakes (add/remove/toggle/first open); reconnect=false just
// snapshots the live pool. Mirrors load_threads_async.
[[nodiscard]] Cmd load_plugins_async(bool reconnect);

// Read the installed skills, approvals, commands and hooks state off the UI
// thread; answers LibraryLoaded, which the reducer stores in m.ui.library.
[[nodiscard]] Cmd load_library();

// The settings pane's add-mode create: a plugin line into the user mcp.json,
// or a starter command/agent file. Writes the disk off the fold and answers
// with SettingsAddDone.
[[nodiscard]] Cmd settings_add(settings::Category concern, std::string line);

// Parse a single thread's JSON off the UI thread. Dispatched from the
// thread picker's Enter handler so the synchronous ~30ms-per-thread
// parse doesn't land between the keypress and the next paint.
// Dispatches `ThreadLoaded{thread}` on success; on failure (file
// vanished, parse error) dispatches a `ThreadLoaded` with an empty
// Thread so the reducer can no-op gracefully without leaving the
// `thread_loading` flag stuck.
[[nodiscard]] Cmd load_thread_async(ThreadId id);

// Re-sync the RAG retriever to `cfg` off the UI thread. An effect, so the
// reducer RETURNS this rather than calling into the retriever itself: update()
// stays a pure function of (Model, Msg), and the work runs where every other
// background job runs — on jaal's pool, cancelled and waited for by the
// kernel's own shutdown. Dispatches nothing; the retriever is its own state.
[[nodiscard]] Cmd apply_rag_settings(store::RagConfig cfg);

// Open a TLS connection to the active provider's host ahead of the first
// request, so the user's first turn skips the handshake.
//
// An EFFECT, so a reducer returns this rather than dialling itself. The target
// is resolved HERE, synchronously, from the selection the reducer just set —
// prewarm_target() is pure — and only the dial runs on the worker. That split
// matters: resolving on the worker would read the active provider at some
// later moment, after a fast second switch may have already changed it.
// Dispatches nothing.
[[nodiscard]] Cmd prewarm_provider(const Model& m);

// Write one change to an mcp.json, off the UI thread, and report back.
//
// The write is a read + JSON rewrite + atomic rename, so a reducer must not
// do it. It returns this instead; the worker performs the matching
// tools::plugin::*() call and dispatches PluginEdited with the outcome, and
// the reducer's handler for PluginEdited does what used to follow the call
// inline (reload the plugin model, toast, close the pane, show a field
// error). `reply` is the request echoed back with the outcome filled in, so
// the handler acts on exactly what was asked even if the pane moved on.
//
// `spec` is consulted only for Add / Update.
[[nodiscard]] Cmd edit_plugin(std::filesystem::path path, PluginEdited reply,
                              tools::plugin::ServerSpec spec = {});

// Write `parent`'s transcript for a fork, off the UI thread, and open its
// directory to the read tool. Replies with ForkTranscriptWritten.
[[nodiscard]] Cmd write_fork_transcript(Thread parent, fork_panel::Choice choice);

// Work out why a clipboard read went unanswered (mosh, tmux config, ssh) and
// reply with ClipboardDiagnosed. Walks /proc and asks tmux, hence a worker.
// In cmd_clipboard_diag.cpp.
[[nodiscard]] Cmd diagnose_clipboard(bool in_ssh);

// Check whether a pasted path names an image file and read it, replying with
// ImagePathSniffed. In cmd_image_paste.cpp.
[[nodiscard]] Cmd sniff_image_path(std::string text);

} // namespace agentty::app::cmd
