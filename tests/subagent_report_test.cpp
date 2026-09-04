// subagent_report_test — locks the `task` tool's report-extraction contract.
//
// Regression target: a subagent that explores straight into its turn budget
// (every completion is a tool call, none ever writes prose) used to have its
// report SALVAGED by a naive reverse-walk that returned the FIRST non-empty
// assistant text — i.e. its turn-1 narration ("I'll start by mapping...").
// The parent then saw a plausible-looking one-liner, concluded the subagent
// "hit its turn cap without a report", and did the work itself. Four parallel
// explorers all came back with their opening sentence instead of an answer.
//
// This test drives the REAL AgenttySubagentRunner (via the registry `task`
// tool) with a SCRIPTED stream seam and asserts:
//
//   A. tool-only-to-the-cap  → report is the "hit its turn budget" banner,
//      NOT the turn-1 narration, and the tool_result is flagged is_error.
//   B. writes prose at the end → report is exactly that final prose, clean.
//   C. the wrap-up nudge fires before the cap so a model that DOES respond to
//      it gets to emit a real report on its last turn.
//   D. a runtime stream works with empty request auth (ChatGPT/local providers).
//   E. a silently empty completion is retried instead of accepted as success.
//   F. cancellation interrupts provider retry backoff immediately.
//   G. permanent errors are not retried.
//   H. interleaved tool-call deltas are assembled by ToolCallId.
//   I. MaxTokens partial text is surfaced as incomplete/error.
//   J. cancellation reaches the active provider Request token.

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>

#include <unistd.h>   // chdir, getpid

namespace fs = std::filesystem;

#include <nlohmann/json.hpp>

#include "agentty/provider/selection.hpp"
#include "agentty/provider/stream_epilogue.hpp"
#include "agentty/provider/anthropic/transport.hpp"
#include "agentty/tool/mcp_tools_bridge.hpp"
#include "agentty/tool/registry.hpp"
#include "agentty/tool/subagent.hpp"
#include "agentty/runtime/msg.hpp"

using nlohmann::json;
using namespace agentty;

namespace {

int g_checks = 0;
int g_fails  = 0;

void check(bool ok, const std::string& what) {
    ++g_checks;
    if (ok) std::printf("ok:   %s\n", what.c_str());
    else  { std::printf("FAIL: %s\n", what.c_str()); ++g_fails; }
}

bool has(const std::string& hay, std::string_view needle) {
    return hay.find(needle) != std::string::npos;
}

// Drive the `task` tool through the production registry, exactly as the
// reducer does. `is_error` is set when the dispatch surfaced a ToolError (the
// runner's is_error path re-wraps as an unexpected through the bridge).
struct TaskOut { std::string text; bool is_error = false; };

TaskOut run_task(const std::string& prompt) {
    const auto* td = tools::find("task");
    if (!td) return {"[task tool missing from registry]", true};
    json args; args["prompt"] = prompt;
    auto r = td->execute(args);         // production dispatch, as cmd_factory does
    if (!r) return {r.error().detail, true};
    return {r->text, false};
}

// A scripted stream seam. `script(turn, req)` decides what THIS completion
// emits; it drives the loop deterministically without any network.
using Script = std::function<void(int turn, const provider::Request&,
                                  const provider::EventSink&)>;

void install_scripted_stream(Script script, bool with_auth = true) {
    auto counter = std::make_shared<std::atomic<int>>(0);
    tools::subagent::Config cfg;
    if (with_auth) cfg.auth = auth::ApiKeyHeader{"sk-test-not-real"};
    cfg.model = "test-model";
    cfg.stream = [counter, script](provider::Request req,
                                   provider::EventSink sink) {
        int turn = counter->fetch_add(1);
        provider::StreamResult result;
        provider::EventSink recording = [&](Msg m) {
            if (auto* sm = std::get_if<msg::StreamMsg>(&m)) {
                std::visit([&](const auto& e) {
                    using T = std::decay_t<decltype(e)>;
                    if constexpr (std::same_as<T, StreamFinished>) {
                        result.end = provider::StreamEnd::AlreadyTerminated;
                        result.stop = e.stop_reason;
                    } else if constexpr (std::same_as<T, StreamError>) {
                        result.end = provider::StreamEnd::TransportError;
                        result.error = e.message;
                        result.retry_after = e.retry_after;
                        result.http_status = e.http_status;
                        result.non_replayable = e.non_replayable;
                    }
                }, *sm);
            }
            sink(std::move(m));
        };
        script(turn, req, recording);
        return result;
    };
    tools::subagent::install(std::move(cfg));
}

// Convenience emitters mirroring the transport → Msg mapping the runner reads.
void emit_text(const provider::EventSink& sink, const std::string& t) {
    sink(Msg{msg::StreamMsg{StreamTextDelta{t}}});
}
void emit_tool_call(const provider::EventSink& sink, const std::string& id,
                    const std::string& name, const json& jargs) {
    sink(Msg{msg::StreamMsg{StreamToolUseStart{ToolCallId{id}, ToolName{name}}}});
    sink(Msg{msg::StreamMsg{StreamToolUseDelta{ToolCallId{id}, jargs.dump()}}});
    sink(Msg{msg::StreamMsg{StreamToolUseEnd{ToolCallId{id}}}});
}
void emit_finish(const provider::EventSink& sink,
                 StopReason r = StopReason::EndTurn) {
    sink(Msg{msg::StreamMsg{StreamFinished{r}}});
}
void emit_error(const provider::EventSink& sink, std::string message,
                bool non_replayable = false) {
    sink(Msg{msg::StreamMsg{StreamError{
        .message = std::move(message),
        .non_replayable = non_replayable,
    }}});
}

// Did the runner ever hand this completion a wrap-up nudge? The nudge is the
// last user message; detect it by its distinctive text.
bool req_has_nudge(const provider::Request& req) {
    for (const auto& m : req.messages)
        if (m.role == Role::User && has(m.text, "almost out of turn budget"))
            return true;
    return false;
}

} // namespace

int main() {
    tools::wire_mcp_runtime("off");   // no bwrap wrapping — CI portability
    // Run from an EMPTY temp directory. This test drives the REAL subagent
    // runner, which executes REAL grep tool calls — each one spawns ripgrep
    // against the tool's workspace root (the process cwd). Left in the source
    // tree, every one of the dozens of greps across all scenarios scans the
    // whole 14 GB working tree; solo that is ~60 s, and under `ctest -j` it
    // blew past the 60 s timeout and looked like a hang. The greps are
    // deliberately no-match probes ("zzz_no_match_expected"), so an empty
    // cwd tests the same report-extraction contract while each rg returns
    // in microseconds — fast AND independent of where the suite is run.
    {
        std::error_code ec;
        const auto dir = fs::temp_directory_path(ec)
            / ("agentty-subagent-test-" + std::to_string(::getpid()));
        fs::create_directories(dir, ec);
        if (!ec) (void)!chdir(dir.c_str());
    }
    // Select a non-Anthropic provider: the runner only calls the disk-reading
    // fresh_auth_header() when active().kind == Anthropic. The OpenAI-family
    // kind (covers Ollama/OpenAI-compat) uses cfg.auth verbatim, and our
    // scripted stream ignores it entirely.
    {
        provider::Selection sel;
        sel.kind = provider::Kind::OpenAI;
        provider::select(sel);
    }

    // ── A. Tool-only to the cap: never write prose. ─────────────────────
    // Every completion emits ONE grep call (harmless, read-only) with empty
    // text, so ran_a_tool stays true and the loop runs to the role budget. The
    // turn-1 completion additionally emits a narration line — the exact bait
    // the old reverse-walk would have returned.
    {
        install_scripted_stream([](int turn, const provider::Request&,
                                   const provider::EventSink& sink) {
            if (turn == 0)
                emit_text(sink, "I'll start by mapping the codebase structure.");
            emit_tool_call(sink, "t" + std::to_string(turn), "grep",
                           json{{"pattern", "zzz_no_match_expected"}});
            emit_finish(sink, StopReason::ToolUse);
        });
        auto out = run_task("explore everything in exhaustive detail");
        check(has(out.text, "turn budget"),
              "A: report is the turn-budget banner");
        // The regression: turn-1 narration must NOT stand alone as the report.
        // If it appears at all it MUST be under the stale-salvage label, after
        // the incompleteness banner — never as the whole answer.
        if (has(out.text, "I'll start by mapping")) {
            check(has(out.text, "early, incomplete")
                  && out.text.find("turn budget")
                       < out.text.find("I'll start by mapping"),
                  "A: narration only appears labeled as a stale partial, after the banner");
        } else {
            check(true, "A: turn-1 narration is not surfaced at all");
        }
        // The banner must clearly signal incompleteness so the parent trusts
        // it instead of the stale one-liner.
        check(has(out.text, "incomplete") || has(out.text, "without producing"),
              "A: banner marks the report incomplete");
    }

    // ── B. Writes prose at the end: clean final report. ─────────────────
    // Explore for a couple of turns, then on turn 3 stop calling tools and
    // write the real answer. The runner must return exactly that prose.
    {
        install_scripted_stream([](int turn, const provider::Request&,
                                   const provider::EventSink& sink) {
            if (turn < 3) {
                emit_tool_call(sink, "t" + std::to_string(turn), "grep",
                               json{{"pattern", "zzz_no_match_expected"}});
                emit_finish(sink, StopReason::ToolUse);
            } else {
                emit_text(sink, "FINAL_ANSWER: the corpus has three modules.");
                emit_finish(sink, StopReason::EndTurn);
            }
        });
        auto out = run_task("summarize the corpus");
        check(has(out.text, "FINAL_ANSWER: the corpus has three modules."),
              "B: clean final prose is returned verbatim");
        check(!has(out.text, "turn budget"),
              "B: no budget banner when the agent finished properly");
        check(!out.is_error, "B: a clean finish is not an error");
    }

    // ── P. Turn budgets are sized to the ROLE's shape of work. ─────────
    // A single global cap could not serve both: read-only sweeps converge
    // fast, while an implementation loop is read → edit → build → read the
    // errors → fix → re-run, which is 6-10 turns for ONE honest cycle and
    // several cycles for a real task. The old flat 24 meant a coder spent
    // its whole budget orienting and reported "I ran out of turns" instead
    // of doing the work — the exact failure the N/O cases above catch.
    //
    // These are CAPS, not quotas: finishing in 5 turns costs 5. So the only
    // runs that pay for a higher ceiling are the ones that would otherwise
    // have FAILED at it.
    {
        namespace sa = agentty::tools::subagent;
        check(sa::max_turns_for(/*read_only=*/false)
                  > sa::max_turns_for(/*read_only=*/true),
              "P: write roles get a strictly larger budget than read-only ones");
        // A coder must be able to run several full edit→build→verify cycles.
        // At ~8 turns per cycle this is the floor for "more than a token
        // attempt" — below it the cap, not the task, decides the outcome.
        check(sa::max_turns_for(/*read_only=*/false) >= 48,
              "P: a write role can run several edit→build→verify cycles");
        // Read-only work still needs room for a real sweep (repo_map, a
        // dozen greps, targeted reads) without being generous for its own
        // sake — an explorer still going at turn 40 is lost, not thorough.
        check(sa::max_turns_for(/*read_only=*/true) >= 24,
              "P: read-only roles keep at least the historical budget");
        // The ceiling is a runaway guard and must bound every role.
        check(sa::kMaxTurns >= sa::max_turns_for(/*read_only=*/false),
              "P: the global ceiling bounds the largest role budget");
    }

    // ── N. Self-reported failure must NOT render as success. ───────────
    // THE bug this case exists for: an agent burns every turn, then signs off
    // with a well-formed report whose content says it did nothing. The old
    // exit path only inspected whether prose EXISTED, so this fell through
    // every guard — no banner, is_error unset — and the UI drew a green
    // "✓ DONE" over "Task NOT completed … made zero edits".
    //
    // Two independent signals must each be enough on their own: hitting the
    // cap, and the report's own verdict.
    {
        install_scripted_stream([](int turn, const provider::Request&,
                                   const provider::EventSink& sink) {
            // Burn the budget on tool calls, then close with a tidy report
            // that admits failure — exactly the observed shape.
            if (turn < agentty::tools::subagent::max_turns_for(/*read_only=*/false) - 1) {   // final turns write prose
                emit_tool_call(sink, "t" + std::to_string(turn), "grep",
                               json{{"pattern", "zzz_unique_" + std::to_string(turn)}});
                emit_finish(sink, StopReason::ToolUse);
            } else {
                emit_text(sink,
                          "## OUTCOME\n\n**Task NOT completed.** I exhausted my "
                          "turn budget on file reading and made **zero edits** — "
                          "no files were created or modified.");
                emit_finish(sink, StopReason::EndTurn);
            }
        });
        auto out = run_task("extract the codec into a shared module");
        check(out.is_error,
              "N: a self-declared failure is flagged is_error (no green ✓ DONE)");
        // The agent's own words are still worth surfacing — the caller needs
        // to know HOW it failed, not just that it did.
        check(has(out.text, "zero edits"),
              "N: the agent's own account is preserved, not discarded");
        check(has(out.text, "turn budget"),
              "N: exhausting the budget is stated explicitly");
    }

    // ── O. Clean prose + exhausted budget is still not a success. ───────
    // Same cap exhaustion, but the closing prose sounds POSITIVE. The report
    // is kept verbatim (it may well be useful), yet the turn cap alone means
    // the task cannot be assumed complete: an agent that ran out of room is
    // not the same as an agent that finished.
    {
        install_scripted_stream([](int turn, const provider::Request&,
                                   const provider::EventSink& sink) {
            if (turn < agentty::tools::subagent::max_turns_for(/*read_only=*/false) - 1) {   // final turns write prose
                emit_tool_call(sink, "t" + std::to_string(turn), "grep",
                               json{{"pattern", "zzz_no_match_expected"}});
                emit_finish(sink, StopReason::ToolUse);
            } else {
                emit_text(sink, "I mapped the modules and it all looks fine.");
                emit_finish(sink, StopReason::EndTurn);
            }
        });
        auto out = run_task("map the modules");
        check(out.is_error,
              "O: hitting the cap is an error even when the prose sounds happy");
        check(has(out.text, "I mapped the modules"),
              "O: the final prose is preserved for the caller to judge");
        check(has(out.text, "incomplete") || has(out.text, "turn budget"),
              "O: the caller is told the result may be incomplete");
    }

    // ── C. The wrap-up nudge fires before the cap. ──────────────────────
    // Stay tool-only, but record whether any completion was handed the nudge.
    // A model that respected it would emit its report; we prove the nudge is
    // delivered at all (the mechanism that makes the report possible).
    {
        auto saw_nudge = std::make_shared<std::atomic<bool>>(false);
        install_scripted_stream([saw_nudge](int turn, const provider::Request& req,
                                            const provider::EventSink& sink) {
            if (req_has_nudge(req)) saw_nudge->store(true);
            emit_tool_call(sink, "t" + std::to_string(turn), "grep",
                           json{{"pattern", "zzz_no_match_expected"}});
            emit_finish(sink, StopReason::ToolUse);
        });
        (void)run_task("keep exploring forever");
        check(saw_nudge->load(),
              "C: a wrap-up nudge is delivered before the turn cap");
    }

    // ── D. Native/local provider streams do not require request auth. ──────
    {
        install_scripted_stream([](int, const provider::Request& req,
                                   const provider::EventSink& sink) {
            check(auth::is_empty(req.auth),
                  "D: authless provider receives an empty request header");
            emit_text(sink, "AUTHLESS_OK");
            emit_finish(sink);
        }, /*with_auth=*/false);
        auto out = run_task("answer using the native provider");
        check(!out.is_error && has(out.text, "AUTHLESS_OK"),
              "D: task runs with native/local auth resolved by transport");
    }

    // ── E. Empty provider completion is retried. ─────────────────────────
    {
        install_scripted_stream([](int turn, const provider::Request&,
                                   const provider::EventSink& sink) {
            if (turn == 0) {
                emit_finish(sink);  // no text, no tool, no StreamError
            } else {
                emit_text(sink, "RECOVERED_AFTER_EMPTY");
                emit_finish(sink);
            }
        });
        auto out = run_task("recover from a dropped response");
        check(!out.is_error && has(out.text, "RECOVERED_AFTER_EMPTY"),
              "E: silently empty completion is retried");
    }

    // ── F. Cancellation interrupts retry backoff. ────────────────────────
    {
        auto cancelled = std::make_shared<std::atomic<bool>>(false);
        tools::cancellation::set([cancelled] { return cancelled->load(); });
        install_scripted_stream([cancelled](int, const provider::Request&,
                                            const provider::EventSink& sink) {
            emit_error(sink, "temporary network failure");
            cancelled->store(true);
        });
        const auto began = std::chrono::steady_clock::now();
        auto out = run_task("cancel during retry backoff");
        const auto elapsed = std::chrono::steady_clock::now() - began;
        tools::cancellation::clear();
        check(out.is_error && has(out.text, "cancelled"),
              "F: cancellation returns an actionable retry error");
        check(elapsed < std::chrono::milliseconds(500),
              "F: cancellation does not wait through retry backoff");
    }

    // ── G. Permanent errors stop immediately without retry. ──────────────
    {
        auto calls = std::make_shared<std::atomic<int>>(0);
        install_scripted_stream([calls](int, const provider::Request&,
                                        const provider::EventSink& sink) {
            calls->fetch_add(1);
            StreamError e{.message = "invalid request payload"};
            e.http_status = 400;
            sink(Msg{msg::StreamMsg{std::move(e)}});
        });
        const auto began = std::chrono::steady_clock::now();
        auto out = run_task("do not retry a bad request");
        check(out.is_error && calls->load() == 1,
              "G: permanent HTTP errors are attempted exactly once");
        check(std::chrono::steady_clock::now() - began
                  < std::chrono::milliseconds(500),
              "G: permanent error returns without retry backoff");
    }

    // ── H. Multiple interleaved calls retain their own argument streams. ──
    {
        auto assembled = std::make_shared<std::atomic<bool>>(false);
        install_scripted_stream([assembled](int turn, const provider::Request& req,
                                            const provider::EventSink& sink) {
            if (turn == 0) {
                sink(Msg{msg::StreamMsg{StreamToolUseStart{
                    ToolCallId{"a"}, ToolName{"grep"}}}});
                sink(Msg{msg::StreamMsg{StreamToolUseStart{
                    ToolCallId{"b"}, ToolName{"grep"}}}});
                sink(Msg{msg::StreamMsg{StreamToolUseDelta{
                    ToolCallId{"a"}, "{\"pattern\":\"alpha"}}});
                sink(Msg{msg::StreamMsg{StreamToolUseDelta{
                    ToolCallId{"b"}, "{\"pattern\":\"beta"}}});
                sink(Msg{msg::StreamMsg{StreamToolUseDelta{
                    ToolCallId{"a"}, "\"}"}}});
                sink(Msg{msg::StreamMsg{StreamToolUseEnd{ToolCallId{"a"}}}});
                sink(Msg{msg::StreamMsg{StreamToolUseDelta{
                    ToolCallId{"b"}, "\"}"}}});
                sink(Msg{msg::StreamMsg{StreamToolUseEnd{ToolCallId{"b"}}}});
                emit_finish(sink, StopReason::ToolUse);
                return;
            }
            for (const auto& m : req.messages) {
                if (m.role != Role::Assistant || m.tool_calls.size() != 2) continue;
                assembled->store(
                    m.tool_calls[0].args.value("pattern", "") == "alpha"
                    && m.tool_calls[1].args.value("pattern", "") == "beta");
            }
            emit_text(sink, "INTERLEAVED_OK");
            emit_finish(sink);
        });
        auto out = run_task("assemble parallel calls");
        check(assembled->load() && has(out.text, "INTERLEAVED_OK"),
              "H: interleaved tool deltas are assembled by call id");
    }

    // ── I. MaxTokens prose is partial, never a clean report. ─────────────
    {
        install_scripted_stream([](int, const provider::Request&,
                                   const provider::EventSink& sink) {
            emit_text(sink, "PARTIAL_NOT_A_REPORT");
            emit_finish(sink, StopReason::MaxTokens);
        });
        auto out = run_task("produce an oversized report");
        check(out.is_error && has(out.text, "max-token limit"),
              "I: MaxTokens completion is reported as an error");
        check(!has(out.text, "\n\nPARTIAL_NOT_A_REPORT"),
              "I: partial MaxTokens prose is not returned as a clean report");
    }

    // ── J. Parent cancellation trips the active provider request token. ──
    {
        auto cancelled = std::make_shared<std::atomic<bool>>(false);
        auto request_cancelled = std::make_shared<std::atomic<bool>>(false);
        tools::cancellation::set([cancelled] { return cancelled->load(); });
        install_scripted_stream([cancelled, request_cancelled](
                                    int, const provider::Request& req,
                                    const provider::EventSink& sink) {
            check(static_cast<bool>(req.cancel),
                  "J: active subagent request has a cancellation token");
            cancelled->store(true);
            const auto until = std::chrono::steady_clock::now()
                             + std::chrono::milliseconds(500);
            while (std::chrono::steady_clock::now() < until
                   && !req.cancel->is_cancelled())
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            request_cancelled->store(req.cancel->is_cancelled());
            emit_error(sink, "cancelled");
        });
        auto out = run_task("cancel active stream");
        tools::cancellation::clear();
        check(request_cancelled->load() && out.is_error,
              "J: parent cancellation reaches provider and stops the task");
    }

    // ── K. An accepted non-idempotent request is never replayed. ─────────
    {
        auto calls = std::make_shared<std::atomic<int>>(0);
        install_scripted_stream([calls](int, const provider::Request&,
                                        const provider::EventSink& sink) {
            calls->fetch_add(1);
            emit_error(sink, "connection closed after server accepted request",
                       /*non_replayable=*/true);
        });
        auto out = run_task("do not duplicate this generation");
        check(calls->load() == 1,
              "K: accepted provider request is attempted exactly once");
        check(out.is_error,
              "K: accepted-request interruption surfaces instead of replaying");
    }

    // ── L. Subagent requests are economical: cached prefix + capped output.
    // Every turn of one subagent must carry the SAME session_key (so the
    // heavy system prompt + tool schemas + tool results prompt-cache across
    // the loop instead of re-billing full price each turn), and max_tokens is
    // capped well below the 32k it used to request (a report doesn't need it).
    {
        auto first_key   = std::make_shared<std::string>();
        auto key_stable  = std::make_shared<std::atomic<bool>>(true);
        auto cap_ok      = std::make_shared<std::atomic<bool>>(true);
        auto key_nonempty= std::make_shared<std::atomic<bool>>(false);
        install_scripted_stream([first_key, key_stable, cap_ok, key_nonempty](
                                    int turn, const provider::Request& req,
                                    const provider::EventSink& sink) {
            if (req.max_tokens > 8192) cap_ok->store(false);
            if (turn == 0) {
                *first_key = req.session_key;
                key_nonempty->store(!req.session_key.empty());
            } else if (req.session_key != *first_key) {
                key_stable->store(false);
            }
            if (turn < 2) {
                emit_tool_call(sink, "t" + std::to_string(turn), "grep",
                               json{{"pattern", "zzz_no_match_expected"}});
                emit_finish(sink, StopReason::ToolUse);
            } else {
                emit_text(sink, "REPORT_OK");
                emit_finish(sink, StopReason::EndTurn);
            }
        });
        auto out = run_task("investigate the module layout");
        check(key_nonempty->load(),
              "L: subagent request carries a stable session_key for caching");
        check(key_stable->load(),
              "L: session_key is identical across the subagent's turns");
        check(cap_ok->load(),
              "L: subagent max_tokens is capped (<=8192), not the old 32k");
        check(!out.is_error && has(out.text, "REPORT_OK"),
              "L: the economical config still produces a clean report");
    }

    // ── M. The subagent system prompt is LEAN. The full parent prompt
    // carries a large memory-tools protocol + skills catalog a subagent can
    // never use (not in its allowlist, no fact persistence). Shipping them
    // just inflates its billed, cached prefix. The lean variant must drop
    // them while keeping the operational discipline it needs.
    {
        const std::string full = provider::default_system_prompt(false);
        const std::string lean = provider::default_system_prompt(true);
        auto has = [](const std::string& s, const char* n) {
            return s.find(n) != std::string::npos;
        };
        check(lean.size() < full.size(),
              "M: lean subagent prompt is smaller than the full parent prompt");
        check(has(full, "<memory-tools>") && !has(lean, "<memory-tools>"),
              "M: lean prompt drops the parent-only memory-tools protocol");
        check(has(lean, "<file-editing>"),
              "M: lean prompt keeps the operational file-editing discipline");
        check(has(lean, "<environment>"),
              "M: lean prompt keeps the environment block");
    }

    std::printf("\n%d checks, %d failures\n", g_checks, g_fails);
    return g_fails == 0 ? 0 : 1;
}
