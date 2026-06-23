// model_caps_test — ModelCapabilities::from_id weak-model inference.
//
// The weak_tool_use heuristic decides which models get the slim prompt +
// doom-loop guards. Lock the classification so a catalog edit can't silently
// flip a hosted/strong model into degraded mode (or vice-versa).

#include <cstdio>
#include <string_view>

#include "agentty/domain/catalog.hpp"

using namespace agentty;

static int g_failures = 0;

#define CHECK(cond)                                                      \
    do {                                                                 \
        if (!(cond)) {                                                   \
            std::fprintf(stderr, "FAIL %s:%d  %s\n",                     \
                         __FILE__, __LINE__, #cond);                     \
            ++g_failures;                                                \
        }                                                                \
    } while (0)

static bool weak(std::string_view id) { return is_weak_model(id); }

static void test_claude_never_weak() {
    // Every hosted Claude family is strong, all generations.
    CHECK(!weak("claude-opus-4-5"));
    CHECK(!weak("claude-sonnet-4-5-20250101"));
    CHECK(!weak("claude-haiku-4-5"));
    CHECK(!weak("claude-3-5-haiku-20241022"));
    CHECK(!weak("claude-opus-4-5[1m]"));      // 1m suffix stripped first
}

static void test_small_local_coder_weak() {
    // The model from the bug report and its small siblings.
    CHECK(weak("qwen2.5-coder:7b"));
    CHECK(weak("qwen2.5-coder:3b"));
    CHECK(weak("qwen2.5:7b"));
    CHECK(weak("codellama:7b"));
    CHECK(weak("deepseek-coder:6.7b"));
    CHECK(weak("phi3:3.8b"));
    CHECK(weak("gemma2:9b"));
    CHECK(weak("starcoder2:7b"));
    CHECK(weak("tinyllama:1.1b"));
    CHECK(weak("smollm2:1.7b"));
}

static void test_large_models_strong() {
    // Large models with NO weak-family signal follow tool schemas reliably.
    CHECK(!weak("llama3.1:70b"));
    CHECK(!weak("mixtral:8x22b"));            // 22b matched, no weak family
}

static void test_weak_family_wins_over_size() {
    // Known weak / coder-only families leak tool JSON at ANY size — the
    // weak-family signal beats the raw >= 14B size shortcut.
    CHECK(weak("qwen2.5-coder:14b"));         // the live case
    CHECK(weak("qwen2.5-coder:32b"));
    CHECK(weak("codellama:34b"));
    CHECK(weak("deepseek-coder:33b"));
}

static void test_tool_trained_families_strong() {
    // Tool-trained families are strong even at smaller sizes.
    CHECK(!weak("qwen3:8b"));
    CHECK(!weak("llama3.1:8b"));
    CHECK(!weak("llama3.3:70b"));
    CHECK(!weak("mistral:7b"));
    CHECK(!weak("mistral-small:24b"));
    CHECK(!weak("ministral:8b"));
    CHECK(!weak("command-r:35b"));
    CHECK(!weak("hermes3:8b"));
    CHECK(!weak("firefunction-v2"));
    CHECK(!weak("functionary-small:7b"));
    CHECK(!weak("devstral:24b"));
    CHECK(!weak("codestral:22b"));
    CHECK(!weak("granite3.1-dense:8b"));
    CHECK(!weak("deepseek-v3"));
    CHECK(!weak("deepseek-r1:32b"));
}

static void test_tiny_strong_family_still_weak() {
    // Even a tool-trained family is unreliable when explicitly tiny (<= 3B).
    CHECK(weak("llama3.1:1b"));
    CHECK(weak("qwen3:1.7b"));               // 1b matched from "1.7b"? see note
    CHECK(weak("granite3.1-moe:3b"));
}

static void test_unknown_id_defaults_strong() {
    // A hosted OpenAI-family id with no size/family signal is assumed capable.
    CHECK(!weak("gpt-4o"));
    CHECK(!weak("gpt-4o-mini"));
    CHECK(!weak("o1-preview"));
    CHECK(!weak("grok-2"));
    CHECK(!weak(""));
    CHECK(!weak("some-random-hosted-model"));
}

static void test_bare_size_signal() {
    // No recognised family but a small size tag → weak; large → strong.
    CHECK(weak("mystery:7b"));
    CHECK(!weak("mystery:70b"));
    // 'b' inside a word (not a param tag) must not be misread.
    CHECK(!weak("turbo-model"));             // 'b' in "turbo" ignored
    CHECK(!weak("bigbird"));                 // no leading digits
}

static void test_max_output_tokens() {
    // Claude 4.x Sonnet/Opus → 64000 (the edit-truncation fix).
    CHECK(max_output_tokens_for("claude-sonnet-4-5") == 64000);
    CHECK(max_output_tokens_for("claude-opus-4-5") == 64000);
    CHECK(max_output_tokens_for("claude-sonnet-4-5-20250101") == 64000);
    CHECK(max_output_tokens_for("claude-opus-4-5[1m]") == 64000);
    // Haiku caps at 8k at every generation.
    CHECK(max_output_tokens_for("claude-haiku-4-5") == 8192);
    CHECK(max_output_tokens_for("claude-3-5-haiku-20241022") == 8192);
    // Legacy 3.x Sonnet/Opus cap at 8k.
    CHECK(max_output_tokens_for("claude-3-5-sonnet-20241022") == 8192);
    CHECK(max_output_tokens_for("claude-3-opus-20240229") == 8192);
    // Non-Claude (local / OpenAI-compat / unknown) → conservative default.
    CHECK(max_output_tokens_for("qwen2.5-coder:7b") == 16384);
    CHECK(max_output_tokens_for("gpt-4o") == 16384);
    CHECK(max_output_tokens_for("") == 16384);
}

int main() {
    test_claude_never_weak();
    test_small_local_coder_weak();
    test_large_models_strong();
    test_weak_family_wins_over_size();
    test_tool_trained_families_strong();
    test_tiny_strong_family_still_weak();
    test_unknown_id_defaults_strong();
    test_bare_size_signal();
    test_max_output_tokens();

    if (g_failures == 0) {
        std::printf("model_caps_test: all checks passed\n");
        return 0;
    }
    std::fprintf(stderr, "model_caps_test: %d check(s) failed\n", g_failures);
    return 1;
}
