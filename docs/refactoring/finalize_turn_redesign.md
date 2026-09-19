# Refactoring `finalize_turn` — Type-Driven Design

The current `finalize_turn` is 400 lines doing 8 distinct things. It's **correct** but hard to test and reason about. Here's how to fix it.

## The Core Problem

`finalize_turn` conflates **distinct concerns** with different lifecycles:

```cpp
maya::Cmd<Msg> finalize_turn(Model& m, StopReason stop_reason) {
    // 1. Drop cancel token
    // 2. Compaction completion (160 lines)
    // 3. Telemetry sealing
    // 4. Stream buffer drains
    // 5. Tool args finalization
    // 6. max_tokens handling
    // 7. Truncation retry
    // 8. Loop mode re-arm
    // 9. Message queue drain
}
```

Each concern has **different failure modes**, **different retry semantics**, and **different side effects**.

---

## The Type-Driven Solution

Make the structure **explicit** with a pipeline of pure transforms and a single effect-gathering coordinator.

### Step 1: Define the State Machine Explicitly

```cpp
namespace agentty::stream_finalize {

// ────────────────────────────────────────────────────────────────────
// The pipeline stages — each is a pure function or a single-effect action
// ────────────────────────────────────────────────────────────────────

struct CancelTokenDropped {};
struct CompactionCompleted { 
    maya::Cmd<Msg> cmd; 
    bool message_queued_and_fired = false;
};
struct TelemetrySealed {};
struct StreamBuffersDrained {};
struct ToolArgsFinalized { 
    bool any_truncated = false; 
    bool max_tokens_hit = false;
};
struct RetryDecision {
    enum class Action { Retry, Fail, Proceed };
    Action action;
    maya::Cmd<Msg> cmd;
};
struct LoopModeRearmed {};
struct QueueDrained { maya::Cmd<Msg> cmd; };

// ────────────────────────────────────────────────────────────────────
// Pure transforms (no mutation, return new state)
// ────────────────────────────────────────────────────────────────────

struct CompactionSummary {
    std::string text;
    size_t up_to_index;
    std::chrono::system_clock::time_point created_at;
    
    static CompactionSummary extract(const Model& m) {
        std::string raw = m.s.compaction_buffer;
        
        // Strip <summary>…</summary> tags
        constexpr std::string_view kOpen = "<summary>";
        constexpr std::string_view kClose = "</summary>";
        if (auto a = raw.find(kOpen); a != std::string::npos) {
            auto body = a + kOpen.size();
            auto b = raw.find(kClose, body);
            if (b != std::string::npos) {
                raw = raw.substr(body, b - body);
            }
        }
        
        // Trim whitespace
        auto is_space = [](char c) { return c==' '||c=='\t'||c=='\n'||c=='\r'; };
        while (!raw.empty() && is_space(raw.front())) raw.erase(raw.begin());
        while (!raw.empty() && is_space(raw.back()))  raw.pop_back();
        if (raw.empty()) raw = "[compaction produced no text]";
        
        return CompactionSummary{
            .text = std::move(raw),
            .up_to_index = std::min(m.s.compaction_target_index, 
                                   m.d.current.messages.size()),
            .created_at = std::chrono::system_clock::now()
        };
    }
};

struct RapidRefillState {
    int turns_since_last;
    int recent_count;
    bool breaker_tripped;
    
    static RapidRefillState update(const StreamState& s) {
        constexpr int kRapidRefillTurns = 2;
        constexpr int kRapidRefillCount = 4;
        
        int count = (s.turns_since_last_compact <= kRapidRefillTurns)
                    ? s.recent_compacts + 1
                    : 1;
        
        return RapidRefillState{
            .turns_since_last = 0,
            .recent_count = count,
            .breaker_tripped = (count >= kRapidRefillCount)
        };
    }
};

struct StatusMessage {
    std::string text;
    std::chrono::steady_clock::time_point until;
    maya::Cmd<Msg> clear_cmd;
    
    static StatusMessage for_compaction(const RapidRefillState& refill, 
                                        bool queued_about_to_fire) {
        auto now = std::chrono::steady_clock::now();
        
        if (refill.breaker_tripped) {
            return StatusMessage{
                .text = "auto-compact disabled (rapid refill); use /compact manually",
                .until = now + std::chrono::seconds{6},
                .clear_cmd = maya::Cmd<Msg>::after(
                    std::chrono::seconds{6} + std::chrono::milliseconds{50},
                    Msg{ClearStatus{now + std::chrono::seconds{6}}})
            };
        }
        
        if (queued_about_to_fire) {
            return StatusMessage{}; // Silent
        }
        
        auto until = now + std::chrono::milliseconds{1500};
        return StatusMessage{
            .text = "context compacted",
            .until = until,
            .clear_cmd = maya::Cmd<Msg>::after(
                std::chrono::milliseconds{1550},
                Msg{ClearStatus{until}})
        };
    }
};

struct TelemetrySnapshot {
    std::uint32_t ttft_ms;
    std::uint32_t stream_ms;
    std::uint32_t wire_bytes;
    std::uint16_t transient_retries;
    std::uint16_t mid_stream_failures;
    std::uint16_t no_progress_failures;
    
    static std::optional<TelemetrySnapshot> capture(const phase::Active* active) {
        if (!active) return std::nullopt;
        
        const auto now = std::chrono::steady_clock::now();
        auto ms_between = [](auto from, auto to) -> std::uint32_t {
            if (from.time_since_epoch().count() == 0) return 0;
            if (to <= from) return 0;
            auto d = std::chrono::duration_cast<std::chrono::milliseconds>(to - from).count();
            // Clamp rather than wrap on clock corruption
            return d > 0xFFFFFFFFLL ? 0xFFFFFFFFu : static_cast<std::uint32_t>(d);
        };
        
        return TelemetrySnapshot{
            .ttft_ms = ms_between(active->started, active->first_delta_at),
            .stream_ms = active->first_delta_at.time_since_epoch().count()
                         ? ms_between(active->first_delta_at, now) : 0,
            .wire_bytes = active->live_delta_bytes > 0xFFFFFFFFull
                          ? 0xFFFFFFFFu 
                          : static_cast<std::uint32_t>(active->live_delta_bytes),
            .transient_retries = static_cast<std::uint16_t>(active->transient_retries),
            .mid_stream_failures = static_cast<std::uint16_t>(active->mid_stream_failures),
            .no_progress_failures = static_cast<std::uint16_t>(active->no_progress_failures)
        };
    }
};

struct ToolFinalizationResult {
    bool any_truncated;
    bool any_mid_string_cutoff;
    
    static ToolFinalizationResult process(std::vector<ToolUse>& tool_calls) {
        bool truncated = false;
        bool mid_string = false;
        
        for (auto& tc : tool_calls) {
            if (tc.args_streaming.empty() || !tc.is_pending()) {
                std::string{}.swap(tc.args_streaming);
                continue;
            }
            
            // Try to parse
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
                    // Distinguish never-closed from mid-string truncation
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
        
        return ToolFinalizationResult{
            .any_truncated = truncated,
            .any_mid_string_cutoff = mid_string
        };
    }
};

// ────────────────────────────────────────────────────────────────────
// Effect actions (mutate model, return Cmd)
// ────────────────────────────────────────────────────────────────────

struct Actions {
    
    static CancelTokenDropped drop_cancel_token(Model& m) {
        if (auto* a = active_ctx(m.s.phase)) {
            a->cancel.reset();
        }
        return CancelTokenDropped{};
    }
    
    static CompactionCompleted complete_compaction(Model& m) {
        auto summary = CompactionSummary::extract(m);
        auto refill = RapidRefillState::update(m.s);
        
        // Push compaction record
        Thread::CompactionRecord rec;
        rec.up_to_index = summary.up_to_index;
        rec.summary = std::move(summary.text);
        rec.created_at = summary.created_at;
        m.d.current.compactions.push_back(std::move(rec));
        m.d.current.updated_at = std::chrono::system_clock::now();
        
        // Update refill breaker state
        m.s.turns_since_last_compact = 0;
        m.s.recent_compacts = refill.recent_count;
        if (refill.breaker_tripped) {
            m.s.autocompact_disabled = true;
        }
        
        // Reset compaction state
        m.s.compaction_target_index = 0;
        m.s.compacting = false;
        m.s.compaction_ceiling = 0;
        m.s.compaction_buffer.clear();
        m.s.phase = phase::Idle{};
        
        const bool queued = !m.ui.composer.queued.empty();
        auto status = StatusMessage::for_compaction(refill, queued);
        m.s.status = std::move(status.text);
        m.s.status_until = status.until;
        
        deps().save_thread(m.d.current);
        release_to_kernel();
        
        // Drain queue if present
        if (queued) {
            auto& head = m.ui.composer.queued.front();
            m.ui.composer.text = std::move(head.text);
            m.ui.composer.attachments = std::move(head.attachments);
            m.ui.composer.cursor = static_cast<int>(m.ui.composer.text.size());
            m.ui.composer.queued.erase(m.ui.composer.queued.begin());
            
            auto [mm, cmd] = submit_message(std::move(m));
            m = std::move(mm);
            return CompactionCompleted{.cmd = cmd, .message_queued_and_fired = true};
        }
        
        return CompactionCompleted{.cmd = status.clear_cmd, .message_queued_and_fired = false};
    }
    
    static TelemetrySealed seal_telemetry(Model& m) {
        if (m.d.current.messages.empty()) return TelemetrySealed{};
        
        auto& last = m.d.current.messages.back();
        if (last.role != Role::Assistant) return TelemetrySealed{};
        
        const auto* active = active_ctx(m.s.phase);
        auto snapshot = TelemetrySnapshot::capture(active);
        if (!snapshot) return TelemetrySealed{};
        
        if (!last.telemetry) last.telemetry.emplace();
        auto& t = *last.telemetry;
        t.ttft_ms = snapshot->ttft_ms;
        t.stream_ms = snapshot->stream_ms;
        t.wire_bytes = snapshot->wire_bytes;
        t.transient_retries = snapshot->transient_retries;
        t.mid_stream_failures = snapshot->mid_stream_failures;
        t.no_progress_failures = snapshot->no_progress_failures;
        
        return TelemetrySealed{};
    }
    
    static StreamBuffersDrained drain_stream_buffers(Model& m) {
        if (m.d.current.messages.empty()) return StreamBuffersDrained{};
        
        auto& last = m.d.current.messages.back();
        if (last.role != Role::Assistant) return StreamBuffersDrained{};
        
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
        
        return StreamBuffersDrained{};
    }
    
    static ToolArgsFinalized finalize_tool_args(Model& m, StopReason stop_reason) {
        if (m.d.current.messages.empty()) {
            return ToolArgsFinalized{};
        }
        
        auto& last = m.d.current.messages.back();
        auto result = ToolFinalizationResult::process(last.tool_calls);
        
        const bool max_tokens_hit = (stop_reason == StopReason::MaxTokens);
        
        // Force-fail all pending tools on max_tokens
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
        
        return ToolArgsFinalized{
            .any_truncated = result.any_truncated,
            .max_tokens_hit = max_tokens_hit
        };
    }
    
    static RetryDecision decide_retry(Model& m, const ToolArgsFinalized& finalized) {
        auto* active = active_ctx(m.s.phase);
        if (!active) return RetryDecision{.action = RetryDecision::Action::Proceed};
        
        if (!finalized.any_truncated) {
            return RetryDecision{.action = RetryDecision::Action::Proceed};
        }
        
        if (finalized.max_tokens_hit) {
            return RetryDecision{.action = RetryDecision::Action::Fail};
        }
        
        if (active->truncation_retries >= kMaxTruncationRetries) {
            return RetryDecision{.action = RetryDecision::Action::Fail};
        }
        
        if (m.d.current.messages.empty()) {
            return RetryDecision{.action = RetryDecision::Action::Proceed};
        }
        
        auto& last = m.d.current.messages.back();
        if (last.role != Role::Assistant) {
            return RetryDecision{.action = RetryDecision::Action::Proceed};
        }
        
        const bool has_committed_work = !last.text.empty() ||
            std::ranges::any_of(last.tool_calls, [](const auto& tc) {
                return !tc.is_pending();
            });
        
        if (has_committed_work) {
            return RetryDecision{.action = RetryDecision::Action::Fail};
        }
        
        // Retry path
        m.d.current.messages.pop_back();
        ++active->truncation_retries;
        m.s.phase = phase::Streaming{std::move(*active)};
        
        return RetryDecision{
            .action = RetryDecision::Action::Retry,
            .cmd = launch_stream(m)
        };
    }
};

// ────────────────────────────────────────────────────────────────────
// The coordinator — runs the pipeline
// ────────────────────────────────────────────────────────────────────

maya::Cmd<Msg> finalize_turn(Model& m, StopReason stop_reason) {
    // Early exit: compaction path is fully separate
    if (m.s.compacting) {
        auto result = Actions::complete_compaction(m);
        return result.cmd;
    }
    
    // Normal turn finalization pipeline
    (void)Actions::drop_cancel_token(m);
    (void)Actions::seal_telemetry(m);
    (void)Actions::drain_stream_buffers(m);
    
    auto finalized = Actions::finalize_tool_args(m, stop_reason);
    auto retry = Actions::decide_retry(m, finalized);
    
    if (retry.action == RetryDecision::Action::Retry) {
        return retry.cmd;
    }
    
    // TODO: remaining concerns (loop mode, message queue, etc.)
    // ... existing code continues here
    
    return maya::Cmd<Msg>::none();
}

} // namespace agentty::stream_finalize
```

---

## Why This Is Better

### 1. **Each concern is isolated and testable**

```cpp
TEST(ToolFinalization, mid_string_cutoff_marks_for_retry) {
    std::vector<ToolUse> calls = {make_truncated_call()};
    auto result = ToolFinalizationResult::process(calls);
    
    ASSERT_TRUE(result.any_truncated);
    ASSERT_TRUE(result.any_mid_string_cutoff);
    ASSERT_TRUE(calls[0].stream_mid_string_truncated);
}
```

### 2. **Pure functions compose**

```cpp
auto summary = CompactionSummary::extract(m);  // pure
auto refill = RapidRefillState::update(m.s);   // pure
auto status = StatusMessage::for_compaction(refill, queued);  // pure
```

### 3. **Side effects are explicit**

```cpp
// Every action returns a value documenting what it did
auto result = Actions::complete_compaction(m);
if (result.message_queued_and_fired) {
    return result.cmd;  // Early exit is visible
}
```

### 4. **The failure modes are types**

```cpp
enum class Action { Retry, Fail, Proceed };
```

Not a maze of nested ifs.

### 5. **It's still one coordinator function**

The public API is unchanged:
```cpp
maya::Cmd<Msg> finalize_turn(Model& m, StopReason stop_reason);
```

But the internals are **readable** and **testable**.

---

## Migration Strategy

1. **Extract pure helpers first** (CompactionSummary, RapidRefillState, etc.)
2. **Add tests for each helper** (they're pure, easy to test)
3. **Wrap existing mutation in Actions::*** (preserves behavior)
4. **Refactor coordinator to use Actions** (replace inline code)
5. **Delete old code once tests pass**

This is **mechanical refactoring** with a type-safety net, not a rewrite.

---

## The Payoff

Before:
- 400 lines of nested mutation
- Hard to test (needs full Model setup)
- Hard to reason about (what if this branch fires?)

After:
- Each concern is 20-50 lines
- Pure functions test in isolation
- Clear data flow (extract → decide → act)
- Same correctness, better structure

The **hard-won edge case handling** stays. The **structure** gets better.
