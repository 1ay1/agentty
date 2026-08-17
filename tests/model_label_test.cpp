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
    expect_label("deepseek-coder:6.7b",     "Deepseek Coder 6.7b");
    expect_label("gemma2:9b",               "Gemma2 9b");

    // ── OpenAI / OpenAI-compat: title-case, keep GPT acronym + version ─
    expect_label("gpt-4o",                  "GPT 4o");
    expect_label("gpt-4o-mini",             "GPT 4o Mini");
    expect_label("gpt-5",                   "GPT 5");
    expect_label("o4-mini",                 "o4 Mini");
    expect_label("chatgpt-4o-latest",       "Chatgpt 4o Latest");

    // ── Provider-namespaced ids (OpenRouter / aggregators) ────────────
    expect_label("openai/gpt-4o-mini",      "GPT 4o Mini");
    expect_label("anthropic/claude-3-haiku","Claude 3 Haiku");
    expect_label("meta-llama/Llama-3.1-8B", "Llama 3.1 8B");
    expect_label("google/gemini-2.0-flash", "Gemini 2.0 Flash");

    // ── Gemini / xAI / DeepSeek hosted ────────────────────────────────
    expect_label("gemini-1.5-pro",          "Gemini 1.5 Pro");
    expect_label("grok-2",                  "Grok 2");
    expect_label("grok-beta",               "Grok Beta");
    expect_label("deepseek-r1",             "Deepseek R1");
    expect_label("deepseek-chat",           "Deepseek Chat");

    // ── agentty `[1m]` extended-context marker is stripped anywhere ───
    expect_label("claude-sonnet-4-5[1m]",   "Claude Sonnet 4 5");
    expect_label("gpt-4o[1m]",              "GPT 4o");

    // ── Acronym preservation + already-cased input ────────────────────
    expect_label("glm-4-9b",                "GLM 4 9b");
    expect_label("Llama-3.1-8B-Instruct",   "Llama 3.1 8B Instruct");

    // ── Degenerate inputs never crash / never empty ───────────────────
    expect_label("",                        "");
    expect_label(":latest",                 "");      // family empty, tag dropped
    expect_label("model",                   "Model");
    expect_label("a",                       "A");
}
