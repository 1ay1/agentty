#include "agentty/provider/chatgpt/provider.hpp"
#include "agentty/domain/bundled_catalog.hpp"
#include "agentty/provider/chatgpt/responses.hpp"
#include "agentty/provider/stream_epilogue.hpp"

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

#include "agentty/provider/wire.hpp"

#include <maya/runtime.hpp>

namespace agentty::provider::chatgpt {

// The Codex provider is a thin, native ChatGPT path: it talks directly to the
// OpenAI Responses backend using the reverse-engineered OAuth login (run
// `agentty login` → ChatGPT). There is NO `codex` binary at runtime and no
// app-server subprocess — authentication, token refresh, and streaming all
// happen in-process, exactly like the Anthropic OAuth path.

struct ChatGptProvider::Impl {};

ChatGptProvider::ChatGptProvider() : impl_(nullptr) {}
ChatGptProvider::~ChatGptProvider() = default;

provider::StreamResult ChatGptProvider::stream(provider::Request req, provider::EventSink sink) {
    if (!responses_available()) {
        sink(StreamStarted{});
        sink(StreamError{
            "Not signed in to ChatGPT. Run `agentty login` and choose "
            "ChatGPT to authenticate, then retry."});
        return provider::StreamResult::failed("not signed in to ChatGPT");
    }
    return stream_responses(std::move(req), std::move(sink));
}

// ── Model discovery ───────────────────────────────────────────────
//
// Mirrors codex-rs: the signed-in ChatGPT account is authoritative about
// which model slugs it will accept, so we FETCH the live `/models` catalog
// (fetch_models) and surface exactly those — never a hardcoded guess. The old
// baked-in list (gpt-5.1-codex …) is what broke: the server stopped offering
// those slugs and rejected the request. The catalog is cached for the process
// so the picker stays instant after the first fetch; a small bundled fallback
// covers the offline / not-signed-in case so a model is always selectable
// (the first turn then surfaces the "run `agentty login`" hint above).
namespace {

std::vector<ModelInfo> bundled_models() {
    // Single bundled catalog. MUST contain only slugs a Codex-enabled ChatGPT
    // account accepts on /responses; the live catalog is the real ceiling.
    return catalog::bundled("chatgpt");
}

// Empty until the first successful live fetch.
using Models = std::vector<ModelInfo>;
maya::guarded<Models>& models_cache() { static maya::guarded<Models> c; return c; }

} // namespace

std::vector<ModelInfo> list_models_cached() {
    return models_cache().read([](const Models& c) { return c; });
}

std::vector<ModelInfo> list_models(Io io) {
    if (auto hit = list_models_cached(); !hit.empty()) return hit;

    // Ask the account for its real catalog (blocking, short timeout; empty on
    // any failure). Only attempted when we actually have a credential.
    std::vector<ModelInfo> resolved;
    if (responses_available()) {
        auto catalog = fetch_models(io);
        // The server lists the default first (or flags is_default); reorder so
        // the account's default is index 0 — that's what the picker preselects.
        std::stable_sort(catalog.begin(), catalog.end(),
            [](const CatalogModel& a, const CatalogModel& b) {
                return a.is_default && !b.is_default;
            });
        for (const auto& cm : catalog) {
            resolved.push_back(ModelInfo{
                ModelId{cm.slug}, cm.display_name, "chatgpt",
                cm.context_window, false, true});
        }
    }

    if (resolved.empty()) resolved = bundled_models();
    else   // cache only real, server-confirmed lists
        models_cache().with([](Models& c, Models fresh) { c = std::move(fresh); }, resolved);
    return resolved;
}

// The account's default model slug (catalog index 0), or a safe fallback when
// the catalog can't be reached. This is what selection/defaulting should use
// instead of any hardcoded "gpt-5.1-codex".
std::string default_model(Io io) {
    auto ms = list_models(io);
    return ms.empty() ? std::string{"gpt-5"} : ms.front().id.value;
}

} // namespace agentty::provider::chatgpt
