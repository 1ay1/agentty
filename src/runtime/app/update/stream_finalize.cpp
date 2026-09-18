// Refactored finalize_turn — same semantics, better structure
//
// The original 400-line function conflated 8+ distinct concerns.
// This version factors each into its own composable piece.

#include <agentty/runtime/app/update/internal.hpp>
#include <agentty/runtime/app/deps.hpp>
#include <agentty/domain/conversation.hpp>
#include <agentty/domain/session.hpp>
#include <agentty/tool/util/partial_json.hpp>
#include <maya/cmd.hpp>
#include <chrono>
#include <algorithm>
#include <ranges>

namespace agentty::detail {

namespace {

// ═══════════════════════════════════════════════════════════════════════════
// Pure Transforms — no mutation, return computed state
// ═══════════════════════════════════════════════════════════════════════════

struct CompactionSummary {
    std::string text;
    std::size_t up_to_index;
    std::chrono::system_clock::time_point created_at;
    
    static CompactionSummary extract_from(std::string raw, 
                                          std::size_t target_index,
                                          std::size_t messages_size) {
        // Strip <summary>…</summary> tags
        constexpr std::string_view kOpen = "<summary>";
        constexpr std::string_view kClose = "</summary>";
        if (auto a_pos = raw.find(kOpen); a_pos != std::string::npos) {
            auto body = a_pos + kOpen.size();
            auto b_pos = raw.find(kClose, body);
            if (b_pos != std::string::npos) {
                raw = raw.substr(body, b_pos - body);
            }
        }
        
        // Trim whitespace
        auto is_space = [](char c) { return c==' '||c=='\t'||c=='\n'||c=='\r'; };
        while (!raw.empty() && is_space(raw.front())) raw.erase(raw.begin());
        while (!raw.empty() && is_space(raw.back()))  raw.pop_back();
        if (raw.empty()) raw = "[compaction produced no text]";
        
        return CompactionSummary{
            .text = std::move(raw),
            .up_to_index = std::min(target_index, messages_size),
            .created_at = std::chrono::system_clock::now()
        };
    }
};

struct RapidRefillDecision {
    int turns_since_last;
    int recent_count;
    bool breaker_tripped;
    
    static RapidRefillDecision from_state(int prev_turns_since, 
                                          int prev_recent) {
        constexpr int kRapidRefillTurns = 2;
        constexpr int kRapidRefillCount = 4;
        
        const int count = (prev_turns_since <= kRapidRefillTurns)
                          ? prev_recent + 1
                          : 1;
        
        return RapidRefillDecision{
            .turns_since_last = 0,
            .recent_count = count,
            .breaker_tripped = (count >= kRapidRefillCount)
        };
    }
};

struct StatusBanner {
    std::string text;
    std::chrono::steady_clock::time_point until;
    maya::Cmd<Msg> clear_cmd;
    
    static StatusBanner for_compaction(bool breaker_tripped,
                                       bool message_queued) {
        auto now = std::chrono::steady_clock::now();
        
        if (breaker_tripped) {
            auto until = now + std::chrono::seconds{6};
            return StatusBanner{
                .text = "auto-compact disabled (rapid refill); use /compact manually",
                .until = until,
                .clear_cmd = maya::Cmd<Msg>::after(
                    std::chrono::seconds{6} + std::chrono::milliseconds{50},
                    Msg{ClearStatus{until}})
            };
        }
        
        if (message_queued) {
            return StatusBanner{}; // Silent — queued message is next visible event
        }
        
        auto until = now + std::chrono::milliseconds{1500};
        return StatusBanner{
            .text = "context compacted",
            .until = until,
            .clear_cmd = maya::Cmd<Msg>::after(
                std::chrono::milliseconds{1550},
                Msg{ClearStatus{until}})
        };
    }
};

struct TelemetryData {
    std::uint32_t ttft_ms;
    std::uint32_t stream_ms;
    std::uint32_t wire_bytes;
    std::uint16_t transient_retries;
    std::uint16_t mid_stream_failures;
    std::uint16_t no_progress_failures;
    
    static std::optional<TelemetryData> capture(const phase::Active* active) {
        if (!active) return std::nullopt;
        
        const auto now = std::chrono::steady_clock::now();
        
        auto ms_between = [](auto from, auto to) -> std::uint32_t {
            if (from.time_since_epoch().count() == 0) return 0;
            if (to <= from) return 0;
            const auto d = std::chrono::duration_cast<
                std::chrono::milliseconds>(to - from).count();
            // Clamp on clock corruption instead of wrapping
            return d > 0xFFFFFFFFLL ? 0xFFFFFFFFu 
                                    : static_cast<std::uint32_t>(d);
        };
        
        return TelemetryData{
            .ttft_ms = ms_between(active->started, active->first_delta_at),
            .stream_ms = active->first_delta_at.time_since_epoch().count()
                         ? ms_between(active->first_delta_at, now)
                         : 0,
            .wire_bytes = active->live_delta_bytes > 0xFFFFFFFFull
                          ? 0xFFFFFFFFu
                          : static_cast<std::uint32_t>(active->live_delta_bytes),
            .transient_retries = static_cast<std::uint16_t>(active->transient_retries),
            .mid_stream_failures = static_cast<std::uint16_t>(active->mid_stream_failures),
            .no_progress_failures = static_cast<std::uint16_t>(active->no_progress_failures)
        };
    }
};

struct ToolFinalizationOutcome {
    bool any_truncated;
    bool any_mid_string_cutoff;
    
    static ToolFinalizationOutcome process_tools(std::vector<ToolUse>& tool_calls) {
        bool truncated = false;
        bool mid_string = false;
        
        for (auto& tc : tool_calls) {
            if (tc.args_streaming.empty() || !tc.is_pending()) {
                std::string{}.swap(tc.args_streaming);
                continue;
            }
            
            // Try parse
            try {
                tc.args = json::parse(tc.args_streaming);
                tc.mark_args_dirty();
            } catch (const std::exception& ex) {
                // Try salvage
                auto salvaged = salvage_args(tc);
                if (!salvaged.empty()) {
                    tc.args = std::move(salvaged);
                    tc.mark_args_dirty();
                } else {
                    // Distinguish truncation types
                    const bool cut_mid_string = 
                        agentty::tools::util::ended_inside_string(tc.args_streaming);
                    
                    if (cut_mid_string) {
                        tc.stream_mid_string_truncated = true;
                        mid_string = true;
                        // Leave Pending for retry
                    } else {
                        auto now = std::chrono::steady_clock::now();
                        tc.status = ToolUse::Failed{
                            tc.started_at(), now,
                            std::string{"tool args never closed: "} + ex.what()
                        };
                    }
                }
            }
            
            std::string{}.swap(tc.args_streaming);
            
            if (tc.is_pending()) {
                if (guard_truncated_tool_args(tc)) truncated = true;
                if (tc.stream_mid_string_truncated) truncated = true;
            }
        }
        
        return ToolFinalizationOutcome{
            .any_truncated = truncated,
            .any_mid_string_cutoff = mid_string
        };
    }
};

// ═══════════════════════════════════════════════════════════════════════════
// Effect Actions — mutate Model, return what happened
// ═══════════════════════════════════════════════════════════════════════════

void drop_cancel_token(Model& m) {
    if (auto* a = active_ctx(m.s.phase)) {
        a->cancel.reset();
    }
}

struct CompactionCompleteResult {
    maya::Cmd<Msg> cmd;
    bool message_fired = false;
};

CompactionCompleteResult complete_compaction(Model& m) {
    auto summary = CompactionSummary::extract_from(
        std::move(m.s.compaction_buffer),
        m.s.compaction_target_index,
        m.d.current.messages.size());
    m.s.compaction_buffer.clear();
    
    // Push compaction record
    Thread::CompactionRecord rec;
    rec.up_to_index = summary.up_to_index;
    rec.summary = std::move(summary.text);
    rec.created_at = summary.created_at;
    m.d.current.compactions.push_back(std::move(rec));
    m.d.current.updated_at = std::chrono::system_clock::now();
    
    // Update rapid-refill state
    auto refill = RapidRefillDecision::from_state(
        m.s.turns_since_last_compact,
        m.s.recent_compacts);
    m.s.turns_since_last_compact = refill.turns_since_last;
    m.s.recent_compacts = refill.recent_count;
    if (refill.breaker_tripped) {
        m.s.autocompact_disabled = true;
    }
    
    // Reset compaction state
    m.s.compaction_target_index = 0;
    m.s.compacting = false;
    m.s.compaction_ceiling = 0;
    m.s.phase = phase::Idle{};
    
    // Set status banner
    const bool queued = !m.ui.composer.queued.empty();
    auto banner = StatusBanner::for_compaction(refill.breaker_tripped, queued);
    m.s.status = std::move(banner.text);
    m.s.status_until = banner.until;
    
    deps().save_thread(m.d.current);
    release_to_kernel();
    
    // Drain queued message if present
    if (queued) {
        auto& head = m.ui.composer.queued.front();
        m.ui.composer.text = std::move(head.text);
        m.ui.composer.attachments = std::move(head.attachments);
        m.ui.composer.cursor = static_cast<int>(m.ui.composer.text.size());
        m.ui.composer.queued.erase(m.ui.composer.queued.begin());
        
        auto [mm, cmd] = submit_message(std::move(m));
        m = std::move(mm);
        return CompactionCompleteResult{.cmd = cmd, .message_fired = true};
    }
    
    return CompactionCompleteResult{.cmd = banner.clear_cmd, .message_fired = false};
}

void seal_telemetry(Model& m) {
    if (m.d.current.messages.empty()) return;
    
    auto& last = m.d.current.messages.back();
    if (last.role != Role::Assistant) return;
    
    const auto* active = active_ctx(m.s.phase);
    auto data = TelemetryData::capture(active);
    if (!data) return;
    
    if (!last.telemetry) last.telemetry.emplace();
    auto& t = *last.telemetry;
    t.ttft_ms = data->ttft_ms;
    t.stream_ms = data->stream_ms;
    t.wire_bytes = data->wire_bytes;
    t.transient_retries = data->transient_retries;
    t.mid_stream_failures = data->mid_stream_failures;
    t.no_progress_failures = data->no_progress_failures;
}

void drain_stream_buffers(Model& m) {
    if (m.d.current.messages.empty()) return;
    
    auto& last = m.d.current.messages.back();
    if (last.role != Role::Assistant) return;
    
    // Drain pending_stream
    if (!last.pending_stream.empty()) {
        last.streaming_text += last.pending_stream;
        last.pending_stream.clear();
    }
    
    // Commit streaming_text
    if (!last.streaming_text.empty()) {
        if (last.text.empty()) last.text = std::move(last.streaming_text);
        else                   last.text += std::move(last.streaming_text);
        std::string{}.swap(last.streaming_text);
    }
    
    // Pre-settle markdown
    if (!last.text.empty()) {
        if (reveal_end_glide_enabled()) {
            auto& cache = m.ui.view_cache.message_md(m.d.current.id, last.id);
            if (!cache.streaming) {
                cache.streaming = std::make_shared<maya::StreamingMarkdown>();
            }
            cache.streaming->set_content(last.text);
            cache.streaming->request_finalize(/*ramp_ms=*/200);
        } else {
            settle_message_md(m, last);
        }
    }
}

struct ToolArgsResult {
    bool any_truncated;
    bool max_tokens_hit;
};

ToolArgsResult finalize_tool_args(Model& m, StopReason stop_reason) {
    if (m.d.current.messages.empty()) {
        return ToolArgsResult{};
    }
    
    auto& last = m.d.current.messages.back();
    auto outcome = ToolFinalizationOutcome::process_tools(last.tool_calls);
    
    const bool max_tokens_hit = (stop_reason == StopReason::MaxTokens);
    
    // Force-fail pending tools on max_tokens
    if (max_tokens_hit && last.role == Role::Assistant) {
        constexpr std::string_view kMsg =
            "Output token cap (max_tokens) was reached before the tool "
            "input finished streaming, so the call was cut off. Even if "
            "the args parsed, the body is likely truncated. Retry with a "
            "smaller payload: prefer `edit` over `write` for long files, "
            "or split the change across multiple calls.";
        
        const auto now = std::chrono::steady_clock::now();
        for (auto& tc : last.tool_calls) {
            if (tc.is_pending()) {
                tc.status = ToolUse::Failed{tc.started_at(), now, std::string{kMsg}};
            } else if (auto* f = std::get_if<ToolUse::Failed>(&tc.status);
                      f && f->output.starts_with("Tool call arguments look incomplete")) {
                f->output.assign(kMsg);
            }
        }
    }
    
    return ToolArgsResult{
        .any_truncated = outcome.any_truncated,
        .max_tokens_hit = max_tokens_hit
    };
}

enum class RetryAction { Retry, Fail, Proceed };

struct RetryDecision {
    RetryAction action;
    maya::Cmd<Msg> cmd;
};

RetryDecision decide_truncation_retry(Model& m, const ToolArgsResult& result) {
    auto* active = active_ctx(m.s.phase);
    if (!active) return RetryDecision{.action = RetryAction::Proceed};
    
    if (!result.any_truncated) {
        return RetryDecision{.action = RetryAction::Proceed};
    }
    
    if (result.max_tokens_hit) {
        return RetryDecision{.action = RetryAction::Fail};
    }
    
    if (active->truncation_retries >= kMaxTruncationRetries) {
        return RetryDecision{.action = RetryAction::Fail};
    }
    
    if (m.d.current.messages.empty()) {
        return RetryDecision{.action = RetryAction::Proceed};
    }
    
    auto& last = m.d.current.messages.back();
    if (last.role != Role::Assistant) {
        return RetryDecision{.action = RetryAction::Proceed};
    }
    
    // Don't retry if we have committed work
    const bool has_committed_work = !last.text.empty() ||
        std::ranges::any_of(last.tool_calls, [](const auto& tc) {
            return !tc.is_pending();
        });
    
    if (has_committed_work) {
        return RetryDecision{.action = RetryAction::Fail};
    }
    
    // Retry: pop placeholder and relaunch
    m.d.current.messages.pop_back();
    ++active->truncation_retries;
    m.s.phase = phase::Streaming{std::move(*active)};
    
    return RetryDecision{
        .action = RetryAction::Retry,
        .cmd = launch_stream(m)
    };
}

// Remaining original logic extracted as separate functions...
// (loop mode re-arm, smart mode cascade, message queue drain, etc.)
// Left as TODO for brevity — same extraction pattern

} // namespace

// ═══════════════════════════════════════════════════════════════════════════
// Public API — coordinator function
// ═══════════════════════════════════════════════════════════════════════════

maya::Cmd<Msg> finalize_turn(Model& m, StopReason stop_reason) {
    // Compaction completion is a separate path
    if (m.s.compacting) {
        auto result = complete_compaction(m);
        return result.cmd;
    }
    
    // Normal turn finalization pipeline
    drop_cancel_token(m);
    seal_telemetry(m);
    drain_stream_buffers(m);
    
    auto tool_result = finalize_tool_args(m, stop_reason);
    auto retry = decide_truncation_retry(m, tool_result);
    
    if (retry.action == RetryAction::Retry) {
        return retry.cmd;
    }
    
// Finalize tool args that failed mid-string truncation after retry budget exhausted
void fail_mid_string_truncated_tools(Model& m) {
    if (m.d.current.messages.empty()) return;
    auto& last = m.d.current.messages.back();
    if (last.role != Role::Assistant) return;
    
    const auto now_ts = std::chrono::steady_clock::now();
    for (auto& tc : last.tool_calls) {
        if (tc.stream_mid_string_truncated && tc.is_pending()) {
            tc.status = ToolUse::Failed{
                tc.started_at(), now_ts,
                "tool args truncated mid-string — the wire cut "
                "off inside a string value (likely `content` / "
                "`command` / `new_text`), so the body is incomplete "
                "and the call was refused. Re-emit the tool with the "
                "full payload — prefer `edit` over `write` for long "
                "files, or split the change across multiple calls."};
        }
        tc.stream_mid_string_truncated = false;
    }
}

void update_rapid_refill_counter(Model& m) {
    // Bump turn counter on every settled turn
    if (m.s.turns_since_last_compact < 1000000) {
        ++m.s.turns_since_last_compact;
    }
    
    // Re-enable auto-compact after quiet stretch
    if (m.s.autocompact_disabled && m.s.turns_since_last_compact > 10) {
        m.s.autocompact_disabled = false;
        m.s.recent_compacts = 0;
    }
}

struct SmartCascadeFeedback {
    int bias_delta = 0;
    
    static SmartCascadeFeedback compute(const Model& m,
                                        smart::Complexity cx,
                                        int delegations,
                                        bool tool_failure) {
        int regret = 0;
        
        // Under-rated: cheap-classified turn that spawned real parallel work
        if (delegations >= 2 && 
            (cx == smart::Complexity::Simple || cx == smart::Complexity::Standard)) {
            regret = +1;
        }
        // Over-rated: Complex-classified turn that delegated nothing
        else if (delegations == 0 && cx == smart::Complexity::Complex) {
            regret = -1;
        }
        
        // Build/test failure is ground truth evidence
        if (tool_failure && regret <= 0) {
            regret = +1;
        }
        
        return SmartCascadeFeedback{.bias_delta = regret};
    }
};

void apply_smart_cascade(Model& m, StopReason stop_reason) {
    const bool tools_pending =
        stop_reason == StopReason::ToolUse ||
        [&] {
            for (auto it = m.d.current.messages.rbegin();
                 it != m.d.current.messages.rend(); ++it) {
                if (it->role != Role::Assistant) break;
                for (const auto& tc : it->tool_calls)
                    if (tc.is_pending() || tc.is_approved()) return true;
            }
            return false;
        }();
    
    // Clear live routing stamp when turn completes
    if (!tools_pending) {
        m.s.smart_turn_model = ModelId{};
        m.s.smart_turn_role.reset();
    }
    
    // CASCADE: run once per user turn at final settle
    if (!tools_pending && m.d.smart.orchestration() && 
        !m.d.current.messages.empty()) {
        
        int delegations = 0;
        bool tool_failure = false;
        for (auto it = m.d.current.messages.rbegin();
             it != m.d.current.messages.rend(); ++it) {
            if (it->role != Role::Assistant) break;
            for (const auto& tc : it->tool_calls) {
                if (tc.name == "task") ++delegations;
                if (std::holds_alternative<ToolUse::Failed>(tc.status) &&
                    (tc.name == "shell" || tc.name == "diagnostics" ||
                     tc.name == "test" || tc.name == "edit"))
                    tool_failure = true;
            }
            if (!it->text.empty() || !it->tool_calls.empty()) break;
        }
        
        const auto cx = m.s.smart_turn_complexity;
        
        // Decay toward neutral first
        if (m.s.smart_effort_bias > 0) --m.s.smart_effort_bias;
        else if (m.s.smart_effort_bias < 0) ++m.s.smart_effort_bias;
        
        auto feedback = SmartCascadeFeedback::compute(m, cx, delegations, tool_failure);
        m.s.smart_effort_bias += feedback.bias_delta;
        
        // Clamp to configured limit
        const int kBiasCap = m.d.smart.bias_clamp;
        if (m.s.smart_effort_bias > kBiasCap) m.s.smart_effort_bias = kBiasCap;
        if (m.s.smart_effort_bias < -kBiasCap) m.s.smart_effort_bias = -kBiasCap;
    }
}

maya::Cmd<Msg> generate_runnable_code_toast(const Model& m) {
    if (m.d.current.messages.empty()) return maya::Cmd<Msg>::none();
    
    const auto& last = m.d.current.messages.back();
    if (last.role != Role::Assistant || last.text.empty()) {
        return maya::Cmd<Msg>::none();
    }
    
    const auto blocks = code_blocks::extract_code_blocks(last.text);
    int runnable = 0;
    for (const auto& b : blocks) {
        if (code_blocks::is_shell_language(b.language)) ++runnable;
    }
    
    if (runnable == 0) return maya::Cmd<Msg>::none();
    
    return set_status_toast(m,
        "▶ " + std::to_string(runnable) +
        (runnable == 1 ? " runnable code block" : " runnable code blocks") +
        " — Ctrl+G to run",
        std::chrono::seconds{6});
}

maya::Cmd<Msg> generate_review_nudge(const Model& m) {
    if (!m.d.pending_changes.empty() && !m.d.show_changes_strip) {
        int files = static_cast<int>(m.d.pending_changes.size());
        return set_status_toast(m,
            "✎ edited " + std::to_string(files) +
            (files == 1 ? " file" : " files") +
            " — Ctrl+R to review",
            std::chrono::seconds{6});
    }
    return maya::Cmd<Msg>::none();
}

void settle_assistant_messages(Model& m) {
    for (std::size_t i = m.ui.frozen_through;
         i < m.d.current.messages.size(); ++i) {
        auto& msg = m.d.current.messages[i];
        if (msg.role != Role::Assistant || msg.text.empty()) continue;
        
        // Skip still-animating reveals
        if (reveal_end_glide_enabled()) {
            const auto* c = m.ui.view_cache.peek(m.d.current.id, msg.id);
            if (c && c->streaming && c->streaming->is_animating())
                continue;
        }
        settle_message_md(m, msg);
    }
    m.ui.pending_settle_freeze = true;
}

struct LoopModeResult {
    maya::Cmd<Msg> cmd;
    bool fired = false;
};

LoopModeResult handle_loop_mode(Model& m) {
    if (!m.s.is_idle() || !m.ui.composer.looping()) {
        return LoopModeResult{};
    }
    
    // Backoff gate — wait for timer
    if (!m.ui.composer.loop_ready(maya::anim::default_clock().now_ms())) {
        return LoopModeResult{};
    }
    
    // Iteration starting from healthy state
    m.ui.composer.loop_note_success();
    ++m.ui.composer.loop_iterations;
    m.ui.composer.text = m.ui.composer.loop_text;
    m.ui.composer.attachments = m.ui.composer.loop_attachments;
    m.ui.composer.cursor = static_cast<int>(m.ui.composer.text.size());
    
    auto [mm, cmd] = submit_message(std::move(m));
    m = std::move(mm);
    
    // Restore armed payload for display
    m.ui.composer.text = m.ui.composer.loop_text;
    m.ui.composer.attachments = m.ui.composer.loop_attachments;
    m.ui.composer.cursor = static_cast<int>(m.ui.composer.text.size());
    
    return LoopModeResult{.cmd = cmd, .fired = true};
}

struct QueueDrainResult {
    maya::Cmd<Msg> cmd;
    bool drained = false;
};

QueueDrainResult drain_message_queue(Model& m) {
    if (!m.s.is_idle() || m.ui.composer.queued.empty()) {
        return QueueDrainResult{};
    }
    
    auto& head = m.ui.composer.queued.front();
    m.ui.composer.text = std::move(head.text);
    m.ui.composer.attachments = std::move(head.attachments);
    m.ui.composer.cursor = static_cast<int>(m.ui.composer.text.size());
    m.ui.composer.queued.erase(m.ui.composer.queued.begin());
    
    auto [mm, cmd] = submit_message(std::move(m));
    m = std::move(mm);
    
    return QueueDrainResult{.cmd = cmd, .drained = true};
}

} // namespace

// ═══════════════════════════════════════════════════════════════════════════
// Public API — coordinator function
// ═══════════════════════════════════════════════════════════════════════════

maya::Cmd<Msg> finalize_turn(Model& m, StopReason stop_reason) {
    // Compaction completion is a separate path
    if (m.s.compacting) {
        auto result = complete_compaction(m);
        return result.cmd;
    }
    
    // Normal turn finalization pipeline
    drop_cancel_token(m);
    seal_telemetry(m);
    drain_stream_buffers(m);
    
    auto tool_result = finalize_tool_args(m, stop_reason);
    auto retry = decide_truncation_retry(m, tool_result);
    
    if (retry.action == RetryAction::Retry) {
        return retry.cmd;
    }
    
    // Remaining finalization steps
    fail_mid_string_truncated_tools(m);
    update_rapid_refill_counter(m);
    apply_smart_cascade(m, stop_reason);
    
    deps().save_thread(m.d.current);
    
    // Batch width profiling
    {
        static const bool prof = [] {
            const char* v = std::getenv("AGENTTY_CACHE_PROF");
            return v && *v && *v != '0';
        }();
        if (prof && !m.d.current.messages.empty() &&
            m.d.current.messages.back().role == Role::Assistant &&
            !m.d.current.messages.back().tool_calls.empty()) {
            static std::FILE* out = std::fopen("/tmp/agentty-cache-prof.log", "a");
            if (out) {
                std::fprintf(out, "[batch] width=%zu\n",
                    m.d.current.messages.back().tool_calls.size());
                std::fflush(out);
            }
        }
    }
    
    auto kick_cmd = cmd::kick_pending_tools(m);
    maya::Cmd<Msg> toast_cmd = maya::Cmd<Msg>::none();
    
    // Queue drain takes precedence over loop mode
    auto queue_result = drain_message_queue(m);
    if (queue_result.drained) {
        return maya::Cmd<Msg>::batch(
            std::vector<maya::Cmd<Msg>>{std::move(kick_cmd), queue_result.cmd});
    }
    
    // Loop mode fires if armed
    auto loop_result = handle_loop_mode(m);
    if (loop_result.fired) {
        return maya::Cmd<Msg>::batch(
            std::vector<maya::Cmd<Msg>>{std::move(kick_cmd), loop_result.cmd});
    }
    
    // Idle settlement
    if (m.s.is_idle()) {
        settle_assistant_messages(m);
        
        // Runnable code toast takes precedence over review nudge
        toast_cmd = generate_runnable_code_toast(m);
        if (toast_cmd.is_none()) {
            toast_cmd = generate_review_nudge(m);
        }
    }
    
    // Batch all final commands
    std::vector<maya::Cmd<Msg>> cmds;
    cmds.push_back(std::move(kick_cmd));
    if (!toast_cmd.is_none()) cmds.push_back(std::move(toast_cmd));
    
    return maya::Cmd<Msg>::batch(std::move(cmds));
}

} // namespace agentty::detail
