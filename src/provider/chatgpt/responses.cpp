// agentty::provider::chatgpt — the ChatGPT/Codex **host** for the shared
// Responses-API codec.
//
// The dialect machinery (conversation → input[], tools[], the SSE state
// machine) used to live here; it was extracted to
// src/provider/responses/codec.cpp once we measured that GitHub Copilot
// speaks the exact same wire. What remains is precisely what is TRUE OF
// CHATGPT AND NOTHING ELSE:
//
//   • the endpoint            chatgpt.com/backend-api/codex/responses
//   • ChatGPT OAuth creds     (auto-refreshing) + chatgpt-account-id
//   • the prompt-cache session id derived from the thread id
//   • `store:false` + include[reasoning.encrypted_content]
//   • Codex-flavoured HTTP error prose ("run `agentty login`…")
//   • the live /models catalog for this account
//
// Everything else is shared. Adding a Responses backend is a Site row like
// the one at the bottom of this file — not a second copy of the codec.
#include "agentty/provider/chatgpt/responses.hpp"
#include "agentty/provider/responses/responses.hpp"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <mutex>
#include <random>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "agentty/provider/chatgpt/codex_oauth.hpp"
#include "agentty/provider/chatgpt/oauth.hpp"
#include "agentty/io/http.hpp"
#include "agentty/util/dbglog.hpp"

namespace agentty::provider::chatgpt {
namespace {
using json = nlohmann::json;

// UTF-8 scrub — a pasted blob / tool output can carry invalid UTF-8 that
// nlohmann::dump() would throw on. Replace malformed bytes with U+FFFD.
// (Local copy: the codec keeps its own for the body it builds; this one is
// only used by the error formatter below.)
std::string scrub_utf8(std::string_view in) {
    std::string out;
    out.reserve(in.size());
    const auto* p = reinterpret_cast<const unsigned char*>(in.data());
    const auto* end = p + in.size();
    while (p < end) {
        unsigned char c = *p;
        int extra = 0;
        if (c < 0x80) { out.push_back(static_cast<char>(c)); ++p; continue; }
        else if ((c & 0xE0) == 0xC0) extra = 1;
        else if ((c & 0xF0) == 0xE0) extra = 2;
        else if ((c & 0xF8) == 0xF0) extra = 3;
        else { out.append("\xEF\xBF\xBD"); ++p; continue; }
        if (p + extra >= end) { out.append("\xEF\xBF\xBD"); ++p; continue; }
        bool ok = true;
        for (int k = 1; k <= extra; ++k)
            if ((p[k] & 0xC0) != 0x80) { ok = false; break; }
        if (!ok) { out.append("\xEF\xBF\xBD"); ++p; continue; }
        out.append(reinterpret_cast<const char*>(p), extra + 1);
        p += extra + 1;
    }
    return out;
}

std::string format_http_error(int status, std::string_view body) {
    const std::string generic = "Codex backend returned HTTP " + std::to_string(status);
    std::string message = generic;

    auto decode = [&](const json& j) -> bool {
        const json* detail = &j;
        std::string tag;
        if (j.is_object() && j.contains("error")) detail = &j["error"];
        if (detail->is_string()) {
            message = detail->get<std::string>();
            return !message.empty();
        }
        if (!detail->is_object()) return false;
        tag = detail->value("type", detail->value("code", std::string{}));
        std::string text = detail->value("message", std::string{});
        if (text.empty() && detail->contains("detail") && (*detail)["detail"].is_string())
            text = (*detail)["detail"].get<std::string>();
        if (text.empty() && detail != &j && j.contains("message") && j["message"].is_string())
            text = j["message"].get<std::string>();
        if (text.empty()) return false;
        message = tag.empty() ? std::move(text) : tag + ": " + text;
        return true;
    };

    bool decoded = false;
    try { decoded = decode(json::parse(body)); }
    catch (const std::exception& e) {
        util::dbglog("chatgpt.http_error.decode", e.what());
    } catch (...) {}

    // Some edge responses retain SSE framing even on an HTTP error. Decode
    // their `data: {...}` payload instead of showing the raw frame.
    if (!decoded) {
        std::size_t pos = 0;
        while (pos < body.size()) {
            const auto nl = body.find('\n', pos);
            std::string_view line = body.substr(pos, nl == std::string_view::npos
                                                        ? std::string_view::npos
                                                        : nl - pos);
            const auto end = nl;
            while (!line.empty() && (line.back() == '\r' || line.back() == ' '))
                line.remove_suffix(1);
            constexpr std::string_view kData = "data:";
            if (line.size() > kData.size() && line.substr(0, kData.size()) == kData) {
                std::string_view payload = line.substr(kData.size());
                while (!payload.empty() && payload.front() == ' ')
                    payload.remove_prefix(1);
                try {
                    if (decode(json::parse(payload))) { decoded = true; break; }
                } catch (...) {}
            }
            if (end == std::string_view::npos) break;
            pos = end + 1;
        }
    }

    if (!decoded && !body.empty()) {
        std::string raw = scrub_utf8(body.substr(0, 1024));
        while (!raw.empty() && (raw.back() == '\r' || raw.back() == '\n' || raw.back() == ' '))
            raw.pop_back();
        if (!raw.empty()) message = generic + ": " + raw;
    }
    if (status == 401)
        message += " — session expired; run `agentty login` and sign in to ChatGPT again";
    return message;
}

std::string new_uuid_v4() {
    static thread_local std::mt19937_64 rng{std::random_device{}()};
    std::uint64_t a = rng(), b = rng();
    a = (a & 0xFFFFFFFFFFFF0FFFULL) | 0x0000000000004000ULL;   // version 4
    b = (b & 0x3FFFFFFFFFFFFFFFULL) | 0x8000000000000000ULL;   // variant 1
    char buf[37];
    std::snprintf(buf, sizeof(buf),
        "%08x-%04x-%04x-%04x-%012llx",
        static_cast<unsigned>(a >> 32),
        static_cast<unsigned>((a >> 16) & 0xFFFF),
        static_cast<unsigned>(a & 0xFFFF),
        static_cast<unsigned>(b >> 48),
        static_cast<unsigned long long>(b & 0xFFFFFFFFFFFFULL));
    return std::string{buf};
}

// Stable per-conversation session id for the Responses backend.
//
// The Codex backend keys prompt caching on this header, so successive turns
// of the SAME conversation that carry the SAME id hit the cached prefix
// (system instructions + tool schemas + prior turns), while a fresh id every
// turn forces a full re-encode — exactly what a random uuid-per-request did,
// silently defeating caching on every hosted ChatGPT turn. codex-rs keeps one
// id for the whole session; we do the same by deriving a deterministic v4-
// shaped uuid from the caller's stable session_key (the thread id). When no
// session_key is supplied (no durable conversation identity) we fall back to a
// random id — correctness first, caching only when we can be stable.
std::string session_id_for(std::string_view session_key) {
    if (session_key.empty()) return new_uuid_v4();
    // FNV-1a over the key → 128 bits, then formatted as a v4 uuid so the
    // backend accepts it. Deterministic: same conversation → same id every
    // turn → cache continuity.
    auto fnv = [](std::string_view s, std::uint64_t seed) {
        std::uint64_t h = seed;
        for (unsigned char c : s) { h ^= c; h *= 0x00000100000001B3ULL; }
        return h;
    };
    std::uint64_t a = fnv(session_key, 0xcbf29ce484222325ULL);
    std::uint64_t b = fnv(session_key, 0x84222325cbf29ce4ULL);
    a = (a & 0xFFFFFFFFFFFF0FFFULL) | 0x0000000000004000ULL;   // version 4
    b = (b & 0x3FFFFFFFFFFFFFFFULL) | 0x8000000000000000ULL;   // variant 1
    char buf[37];
    std::snprintf(buf, sizeof(buf),
        "%08x-%04x-%04x-%04x-%012llx",
        static_cast<unsigned>(a >> 32),
        static_cast<unsigned>((a >> 16) & 0xFFFF),
        static_cast<unsigned>(a & 0xFFFF),
        static_cast<unsigned>(b >> 48),
        static_cast<unsigned long long>(b & 0xFFFFFFFFFFFFULL));
    return std::string{buf};
}

// ── The ChatGPT Site ──────────────────────────────────────────────────────

std::expected<responses::Target, std::string>
chatgpt_authorize(provider::Request& req) {
    auto creds = codex_fresh_credentials();   // auto-refreshes if stale
    if (!creds || creds->access_token.empty())
        return std::unexpected(
            std::string{"not signed in to ChatGPT — run `agentty login` and "
                        "choose ChatGPT, or use --provider with an API key"});

    responses::Target t;
    t.host = "chatgpt.com";
    t.port = 443;
    t.path = "/backend-api/codex/responses";
    t.headers = {
        {"authorization",     "Bearer " + creds->access_token},
        {"content-type",      "application/json"},
        {"accept",            "text/event-stream"},
        {"openai-beta",       "responses=experimental"},
        {"originator",        OAuthConfig::originator},
        {"session_id",        session_id_for(req.session_key)},
        {"user-agent",        std::string("codex_cli_rs/") + OAuthConfig::codex_client_version},
    };
    if (!creds->account_id.empty())
        t.headers.push_back({"chatgpt-account-id", creds->account_id});
    // The hosted Codex backend's default slug when the caller didn't pin one.
    t.model = (req.model.empty() || req.model == "chatgpt-default")
                  ? "gpt-5-codex" : req.model;
    return t;
}

void chatgpt_decorate_body(json& body, const provider::Request&) {
    // The Codex backend runs stateless: we replay the whole conversation each
    // turn, so nothing should be stored server-side.
    body["store"] = false;
    // Ask the backend to return each reasoning item's encrypted_content so we
    // can replay it in input[] next turn. Under store:false this is the ONLY
    // way to carry chain-of-thought across tool rounds — without it a
    // reasoning model re-derives its plan from scratch each round (worse
    // answers, wasted tokens). Mirrors codex-rs.
    body["include"] = json::array({"reasoning.encrypted_content"});
}

const responses::Site kChatGptSite{
    .id                 = "chatgpt",
    .authorize          = &chatgpt_authorize,
    .decorate_body      = &chatgpt_decorate_body,
    .explain_http_error = &format_http_error,
};

} // namespace

bool responses_available() {
    // Called by the provider-picker VIEW once per rendered frame. The naive
    // path (read codex_credentials.json → unseal → JSON parse) costs disk +
    // AES work per frame while the picker is open. Cache the boolean keyed
    // on the file's (mtime, size): a stat is ~1µs and invalidates correctly
    // on sign-in, sign-out, and cross-process refreshes alike.
    namespace fs = std::filesystem;
    static std::mutex mu;
    static bool cached = false;
    static fs::file_time_type cached_mtime{};
    static std::uintmax_t cached_size = static_cast<std::uintmax_t>(-1);
    std::scoped_lock lk(mu);
    std::error_code ec;
    const auto p = codex_credentials_path();
    const auto mtime = fs::last_write_time(p, ec);
    const auto size  = ec ? 0 : fs::file_size(p, ec);
    if (ec) {   // missing/unreadable → signed out; remember that cheaply
        cached = false;
        cached_mtime = {};
        cached_size = static_cast<std::uintmax_t>(-1);
        return false;
    }
    if (mtime != cached_mtime || size != cached_size) {
        auto c = load_codex_credentials();
        cached = c && !c->access_token.empty();
        cached_mtime = mtime;
        cached_size  = size;
    }
    return cached;
}

// ── Live model catalog ────────────────────────────────────────────────────
std::vector<CatalogModel> fetch_models() {
    auto creds = codex_fresh_credentials();
    if (!creds || creds->access_token.empty()) return {};

    http::Request hr;
    hr.method = http::HttpMethod::Get;
    hr.host   = "chatgpt.com";
    hr.port   = 443;
    hr.path   = std::string("/backend-api/codex/models?client_version=")
              + OAuthConfig::codex_client_version;
    hr.headers = {
        {"authorization", "Bearer " + creds->access_token},
        {"accept",        "application/json"},
        {"openai-beta",   "responses=experimental"},
        {"originator",    OAuthConfig::originator},
        {"user-agent",    std::string("codex_cli_rs/") + OAuthConfig::codex_client_version},
    };
    if (!creds->account_id.empty())
        hr.headers.push_back({"chatgpt-account-id", creds->account_id});

    http::Timeouts tos;
    tos.connect = std::chrono::milliseconds(8'000);
    tos.total   = std::chrono::milliseconds(15'000);
    // The catalog is a small JSON list (~a few KB). Cap hard so a misbehaving
    // proxy / replay loop can't stream us into OOM on a routine picker probe
    // (parity with Anthropic's list_models).
    hr.max_body_bytes = 1ull * 1024 * 1024;

    auto result = http::default_client().send(hr, tos);
    if (!result || result->status < 200 || result->status >= 300) return {};

    std::vector<CatalogModel> out;
    try {
        auto j = nlohmann::json::parse(result->body);
        const auto& arr = j.contains("models") ? j["models"] : j;
        if (!arr.is_array()) return {};
        for (const auto& m : arr) {
            // The catalog carries internal models the picker must not show
            // (e.g. `codex-auto-review`, visibility="hide"). Mirror codex-rs:
            // only surface models the account is meant to select. Anything not
            // explicitly listed as visible is dropped.
            const std::string vis = m.value("visibility", std::string{"list"});
            if (vis == "hide" || vis == "hidden") continue;
            CatalogModel cm;
            cm.slug = m.value("slug", m.value("id", std::string{}));
            if (cm.slug.empty()) continue;
            cm.display_name   = m.value("display_name", cm.slug);
            cm.context_window = m.value("context_window", 272000);
            cm.is_default     = m.value("is_default", false);
            out.push_back(std::move(cm));
        }
    } catch (...) {
        return {};
    }
    return out;
}

provider::StreamResult stream_responses(provider::Request req,
                                        provider::EventSink sink) {
    return responses::stream(kChatGptSite, std::move(req), std::move(sink));
}

// ── Test seams ────────────────────────────────────────────────────────────
// These keep their historical ChatGPT-flavoured behaviour: build_body_for_test
// returns the body AS SENT (neutral codec body + this host's decorations), so
// the existing expectations about store/include still hold.
nlohmann::json build_body_for_test(const provider::Request& req) {
    provider::Request r = req;
    json body = responses::build_body(r);
    if (r.model.empty() || r.model == "chatgpt-default")
        body["model"] = "gpt-5-codex";
    chatgpt_decorate_body(body, r);
    return body;
}

std::string session_id_for_test(std::string_view session_key) {
    return session_id_for(session_key);
}

std::string format_http_error_for_test(int status, std::string_view body) {
    return format_http_error(status, body);
}

std::vector<Msg> parse_sse_for_test(const std::vector<std::string>& sse_data_lines) {
    return responses::parse_sse_for_test(sse_data_lines);
}

} // namespace agentty::provider::chatgpt
