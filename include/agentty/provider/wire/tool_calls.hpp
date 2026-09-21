#pragma once
// tool_calls.hpp — WHO a streamed tool-call chunk belongs to.
//
// ════════════════════════════════════════════════════════════════════════
// THE PROBLEM
// ════════════════════════════════════════════════════════════════════════
//
// A streaming chat protocol spreads one tool call across many chunks, and
// several calls can be in flight at once. Attributing a chunk to a call is
// the whole job, and Chat Completions gives you two unreliable handles:
//
//   • `id`    — may be absent on continuations, may be restated as "",
//               and may repeat across parallel calls.
//   • `index` — may be absent entirely, and may be REUSED for a new call
//               once the previous one at that index is done.
//
// Neither alone is identity. Every harness that treated one as identity has
// shipped the same bug: openai-python #3377 (keyed on arrival order), opik
// #8360, strands #3950, crewai, azure-ai, theia, llm-gateway #136, zed
// #42584. Ours did too — index-only keying merged two parallel calls that
// reused index 0, corrupting both sets of arguments.
//
// ════════════════════════════════════════════════════════════════════════
// THE RULE
// ════════════════════════════════════════════════════════════════════════
//
//   Identity is (id, index) JOINTLY. Use whichever the chunk carries, and
//   when it carries neither, FAIL rather than guess.
//
// Failing is the load-bearing part. A wrong guess appends one call's bytes
// to another and produces JSON that parses — so the corruption is silent
// and lands in a tool invocation. An explicit failure is a visible error on
// one turn. The second is strictly better, and nothing about the wire lets
// you avoid choosing.
//
// This is a straight port of the reasoning in Zed's ToolCallAccumulator
// (crates/language_model_core/src/chat_completion.rs), which is the only
// implementation I found that states the joint-identity rule explicitly
// rather than discovering it one bug report at a time.
//
// ════════════════════════════════════════════════════════════════════════
// WHY IT LIVES HERE
// ════════════════════════════════════════════════════════════════════════
//
// Two decoders (openai/transport.cpp, responses/codec.cpp) each grew their
// own attribution logic inline, inside functions that are also doing JSON
// walking and event emission. That is how the two drifted: a fix to one is
// invisible to the other, and neither is testable without a full SSE
// fixture. Here it is a value type with no I/O, so a case from a bug report
// is four lines of test.

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace agentty::provider::wire {

// What a chunk tells us about which call it belongs to. Both halves are
// optional because both genuinely go missing on real wires.
struct ChunkIdentity {
    std::string_view          id;      // "" = absent or restated empty
    std::optional<std::size_t> index;  // nullopt = field absent
};

// Why an attribution failed. A value, not a bool, so the caller can say
// something true to the user instead of "tool call failed".
enum class AttributionError : std::uint8_t {
    // The chunk carried neither id nor index, and more than one call is in
    // flight, so there is no evidence for which one it extends.
    Ambiguous,
};

// One call's accumulated state. The decoder owns the bytes; this owns the
// question of which call they belong to.
struct Call {
    std::string                id;
    std::string                name;
    std::string                args;
    std::optional<std::size_t> index;
    // Set once the decoder has announced this call downstream, so a
    // re-attributed chunk does not re-announce it.
    bool                       started = false;
    bool                       ended   = false;
};

// The result of attributing one chunk: which call it belongs to, and
// whether that call is NEW (the caller must announce it) or an existing one
// being continued.
struct Attribution {
    std::size_t call;        // index into calls()
    bool        is_new;      // a call was created for this chunk
    // The call this chunk DISPLACED, if the server reused an index for a
    // different id. The caller must close it — an unclosed call leaves a
    // tool_use with no tool_result and hangs the turn.
    std::optional<std::size_t> displaced;
};

// Attributes streamed chunks to calls. No I/O, no JSON, no events: hand it
// what the chunk said about identity and it tells you which call that is.
class ToolCallTracker {
public:
    [[nodiscard]] const std::vector<Call>& calls() const noexcept { return calls_; }
    [[nodiscard]] std::vector<Call>&       calls()       noexcept { return calls_; }
    [[nodiscard]] bool empty() const noexcept { return calls_.empty(); }

    // Which call does this chunk belong to?
    //
    // The four cases, in the order the evidence supports:
    //
    //   (id, index)  both present — the strongest handle. An id we know
    //                continues that call even if its index moved; an id we
    //                do not know starts a new call, DISPLACING whatever
    //                held that index (the reuse case).
    //   (id, -)      id alone — match by id, else a new call. This is the
    //                MiniMax shape: identity by id, no usable index.
    //   (-, index)   index alone — match by index, else a new call. The
    //                ordinary continuation chunk.
    //   (-, -)       no evidence. Legal only when exactly one call is in
    //                flight; otherwise Ambiguous.
    [[nodiscard]] std::expected<Attribution, AttributionError>
    attribute(const ChunkIdentity& who) {
        const bool has_id = !who.id.empty();

        if (has_id) {
            // Known id: this call continues, wherever its index now says.
            if (const auto found = find_by_id(who.id)) {
                if (who.index) bind_index(*who.index, *found);
                return Attribution{*found, /*is_new=*/false, std::nullopt};
            }
            // Unknown id. If some other call holds this index, the server
            // has REUSED the index for a new call — the old one is over.
            std::optional<std::size_t> displaced;
            if (who.index) {
                if (const auto at = find_by_index(*who.index);
                    at && !calls_[*at].ended)
                    displaced = at;
            }
            const auto fresh = open(std::string{who.id}, who.index);
            return Attribution{fresh, /*is_new=*/true, displaced};
        }

        if (who.index) {
            if (const auto found = find_by_index(*who.index))
                return Attribution{*found, /*is_new=*/false, std::nullopt};
            const auto fresh = open(std::string{}, who.index);
            return Attribution{fresh, /*is_new=*/true, std::nullopt};
        }

        // No identity at all.
        if (calls_.empty())
            return Attribution{open(std::string{}, std::nullopt), true, std::nullopt};
        if (calls_.size() == 1)
            return Attribution{0, /*is_new=*/false, std::nullopt};
        return std::unexpected(AttributionError::Ambiguous);
    }

    void reset() noexcept {
        calls_.clear();
        by_index_.clear();
    }

private:
    [[nodiscard]] std::optional<std::size_t> find_by_id(std::string_view id) const {
        for (std::size_t i = 0; i < calls_.size(); ++i)
            if (!calls_[i].id.empty() && calls_[i].id == id) return i;
        return std::nullopt;
    }

    [[nodiscard]] std::optional<std::size_t> find_by_index(std::size_t index) const {
        for (const auto& [idx, call] : by_index_)
            if (idx == index) return call;
        return std::nullopt;
    }

    void bind_index(std::size_t index, std::size_t call) {
        calls_[call].index = index;
        for (auto& [idx, c] : by_index_)
            if (idx == index) { c = call; return; }
        by_index_.emplace_back(index, call);
    }

    std::size_t open(std::string id, std::optional<std::size_t> index) {
        calls_.push_back(Call{std::move(id), {}, {}, index, false, false});
        const auto at = calls_.size() - 1;
        if (index) bind_index(*index, at);
        return at;
    }

    std::vector<Call> calls_;
    // Small-N association list rather than a map: a turn has a handful of
    // parallel calls, and a linear scan over 2-4 entries beats a hash.
    std::vector<std::pair<std::size_t, std::size_t>> by_index_;
};

}  // namespace agentty::provider::wire
