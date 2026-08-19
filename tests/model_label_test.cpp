// model_label_test — pretty_model_label() turn-header normalization.
//
// The assistant turn header shows a SHORT, human name for the active
// model. Raw provider ids (`codellama:latest`, `qwen2.5-coder:7b`,
// `openai/gpt-4o-mini`, `claude-sonnet-4-5[1m]`) are long and ugly; this
// pins the cleanup across every real-world id shape we ship against so a
// regression that re-leaks a raw id into the header is caught at CI.

#include "agentty/runtime/view/helpers.hpp"

#include <string>
#include <string_view>

#include "agtest.hpp"

namespace {

void expect_label(std::string_view id, std::string_view want) {
    std::string got = agentty::ui::pretty_model_label(id);
    CHECK_MESSAGE(got == want, "pretty_model_label(\"" << std::string(id)
                                  << "\") got \"" << got << "\" want \""
                                  << std::string(want) << "\"");
}

} // namespace

TEST_CASE("pretty_model_label normalizes real-world ids") {
    // ── Ollama: drop :latest, keep meaningful size/quant tags ──────────
    expect_label("codellama:latest",        "Codellama");
    expect_label("llama3.2:latest",         "Llama3.2");
    expect_label("qwen2.5-coder:7b",        "Qwen2.5 Coder 7b");
    expect_label("llama3.1:70b",            "Llama3.1 70b");
    expect_label("mixtral:8x7b",            "Mixtral 8x7b");
    expect_label("phi3:3.8b",               "Phi3 3.8b");
    expect_label("deepseek-coder:6.7b",     "DeepSeek Coder 6.7b");
    expect_label("gemma2:9b",               "Gemma2 9b");

    // ── OpenAI / OpenAI-compat: title-case, keep GPT acronym + version ─
    expect_label("gpt-4o",                  "GPT 4o");
    expect_label("gpt-4o-mini",             "GPT 4o Mini");
    expect_label("gpt-5",                   "GPT 5");
    expect_label("o4-mini",                 "o4 Mini");
    expect_label("chatgpt-4o-latest",       "ChatGPT 4o");     // brand case + alias drop
    expect_label("gpt-4o-2024-08-06",       "GPT 4o");         // snapshot triple drop
    expect_label("gpt-4.1-nano",            "GPT 4.1 Nano");
    expect_label("gpt-5.1-codex-max",       "GPT 5.1 Codex Max");
    expect_label("codex-mini-latest",       "Codex Mini");

    // ── Provider-namespaced ids (OpenRouter / aggregators) ────────────
    expect_label("openai/gpt-4o-mini",      "GPT 4o Mini");
    expect_label("anthropic/claude-3-haiku","Claude 3 Haiku");
    expect_label("meta-llama/Llama-3.1-8B", "Llama 3.1 8B");
    expect_label("google/gemini-2.0-flash", "Gemini 2.0 Flash");

    // ── Gemini / xAI / DeepSeek hosted ────────────────────────────
    expect_label("gemini-1.5-pro",          "Gemini 1.5 Pro");
    expect_label("grok-2",                  "Grok 2");
    expect_label("grok-beta",               "Grok Beta");
    expect_label("deepseek-r1",             "DeepSeek R1");    // brand case
    expect_label("deepseek-chat",           "DeepSeek Chat");

    // ── Claude: adjacent version digits join with a dot; snapshots drop ─
    expect_label("claude-sonnet-4-5",       "Claude Sonnet 4.5");
    expect_label("claude-opus-4-1",         "Claude Opus 4.1");
    expect_label("claude-3-5-haiku-20241022", "Claude 3.5 Haiku");
    expect_label("claude-sonnet-4-20250514",  "Claude Sonnet 4");

    // ── agentty `[1m]`/`[2m]` extended-context markers are stripped ───
    expect_label("claude-sonnet-4-5[1m]",   "Claude Sonnet 4.5");
    expect_label("claude-opus-4-5[2m]",     "Claude Opus 4.5");
    expect_label("gpt-4o[1m]",              "GPT 4o");

    // ── Ollama quant tags: keep size + variant, drop quant noise ──────
    expect_label("llama3.3:70b-instruct-q4_K_M", "Llama3.3 70b Instruct");
    expect_label("phi4:Q8_0",               "Phi4");
    expect_label("llama3:8b-fp16",          "Llama3 8b");

    // ── Acronym preservation + already-cased input ────────────────────
    expect_label("glm-4-9b",                "GLM 4 9b");
    expect_label("Llama-3.1-8B-Instruct",   "Llama 3.1 8B Instruct");

    // ── Degenerate inputs never crash / never empty ───────────────────
    expect_label("",                        "");
    expect_label(":latest",                 "");      // family empty, tag dropped
    expect_label("model",                   "Model");
    expect_label("a",                       "A");
}
