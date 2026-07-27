#include "agentty/provider/codex_cli/provider.hpp"
#include "agentty/provider/codex_cli/responses.hpp"

#include <utility>
#include <vector>

#include "agentty/provider/wire.hpp"

namespace agentty::provider::codex_cli {

// The Codex provider is a thin, native ChatGPT path: it talks directly to the
// OpenAI Responses backend using the reverse-engineered OAuth login (run
// `agentty login` → ChatGPT). There is NO `codex` binary at runtime and no
// app-server subprocess — authentication, token refresh, and streaming all
// happen in-process, exactly like the Anthropic OAuth path.

struct CodexCliProvider::Impl {};

CodexCliProvider::CodexCliProvider() : impl_(nullptr) {}
CodexCliProvider::~CodexCliProvider() = default;

void CodexCliProvider::stream(provider::Request req, provider::EventSink sink) {
    if (!responses_available()) {
        sink(StreamStarted{});
        sink(StreamError{
            "Not signed in to ChatGPT. Run `agentty login` and choose "
            "ChatGPT to authenticate, then retry."});
        return;
    }
    stream_responses(std::move(req), std::move(sink));
}

// ── Model discovery ────────────────────────────────────────────────────────
//
// Signed in with ChatGPT OAuth → advertise the Codex model line-up. When not
// signed in, return the same list so the model is still selectable; the first
// turn then surfaces the "run `agentty login`" hint above.
std::vector<ModelInfo> list_models() {
    return {
        {ModelId{"gpt-5.1-codex"},      "GPT-5.1 Codex",      "codex-cli", 272000, false, true},
        {ModelId{"gpt-5.1-codex-mini"}, "GPT-5.1 Codex mini", "codex-cli", 272000, false, true},
        {ModelId{"gpt-5-codex"},        "GPT-5 Codex",        "codex-cli", 272000, false, true},
        {ModelId{"gpt-5"},              "GPT-5",              "codex-cli", 272000, false, true},
    };
}

} // namespace agentty::provider::codex_cli
