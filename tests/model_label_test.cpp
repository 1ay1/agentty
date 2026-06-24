// model_label_test — pretty_model_label() turn-header normalization.
//
// The assistant turn header shows a SHORT, human name for the active
// model. Raw provider ids (`codellama:latest`, `qwen2.5-coder:7b`,
// `openai/gpt-4o-mini`, `claude-sonnet-4-5[1m]`) are long and ugly; this
// pins the cleanup across every real-world id shape we ship against so a
// regression that re-leaks a raw id into the header is caught at CI.

#include "agentty/runtime/view/helpers.hpp"

#include <cstdio>
#include <string>
#include <string_view>

namespace {

int failures = 0;

void check(std::string_view id, std::string_view want) {
    std::string got = agentty::ui::pretty_model_label(id);
    if (got != want) {
        std::fprintf(stderr, "FAIL  pretty_model_label(\"%.*s\")\n"
                             "        got  \"%s\"\n"
                             "        want \"%.*s\"\n",
                     static_cast<int>(id.size()), id.data(),
                     got.c_str(),
                     static_cast<int>(want.size()), want.data());
        ++failures;
    }
}

} // namespace

int main() {
    // ── Ollama: drop :latest, keep meaningful size/quant tags ──────────
    check("codellama:latest",        "Codellama");
    check("llama3.2:latest",         "Llama3.2");
    check("qwen2.5-coder:7b",        "Qwen2.5 Coder 7b");
    check("llama3.1:70b",            "Llama3.1 70b");
    check("mixtral:8x7b",            "Mixtral 8x7b");
    check("phi3:3.8b",               "Phi3 3.8b");
    check("deepseek-coder:6.7b",     "Deepseek Coder 6.7b");
    check("gemma2:9b",               "Gemma2 9b");

    // ── OpenAI / OpenAI-compat: title-case, keep GPT acronym + version ─
    check("gpt-4o",                  "GPT 4o");
    check("gpt-4o-mini",             "GPT 4o Mini");
    check("gpt-5",                   "GPT 5");
    check("o4-mini",                 "o4 Mini");
    check("chatgpt-4o-latest",       "Chatgpt 4o Latest");

    // ── Provider-namespaced ids (OpenRouter / aggregators) ────────────
    check("openai/gpt-4o-mini",      "GPT 4o Mini");
    check("anthropic/claude-3-haiku","Claude 3 Haiku");
    check("meta-llama/Llama-3.1-8B", "Llama 3.1 8B");
    check("google/gemini-2.0-flash", "Gemini 2.0 Flash");

    // ── Gemini / xAI / DeepSeek hosted ────────────────────────────────
    check("gemini-1.5-pro",          "Gemini 1.5 Pro");
    check("grok-2",                  "Grok 2");
    check("grok-beta",               "Grok Beta");
    check("deepseek-r1",             "Deepseek R1");
    check("deepseek-chat",           "Deepseek Chat");

    // ── agentty `[1m]` extended-context marker is stripped anywhere ───
    check("claude-sonnet-4-5[1m]",   "Claude Sonnet 4 5");
    check("gpt-4o[1m]",              "GPT 4o");

    // ── Acronym preservation + already-cased input ────────────────────
    check("glm-4-9b",                "GLM 4 9b");
    check("Llama-3.1-8B-Instruct",   "Llama 3.1 8B Instruct");

    // ── Degenerate inputs never crash / never empty ───────────────────
    check("",                        "");
    check(":latest",                 "");      // family empty, tag dropped
    check("model",                   "Model");
    check("a",                       "A");

    if (failures == 0) {
        std::puts("model_label_test: OK");
        return 0;
    }
    std::fprintf(stderr, "model_label_test: %d failure(s)\n", failures);
    return 1;
}
