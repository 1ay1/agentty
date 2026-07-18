// agentty::rag — multi-query expansion (RAG-Fusion) via Ollama /api/generate.
// One cheap non-streaming local LLM call; degrades to {query} on any failure.
// See expand.hpp for rationale.

#include "agentty/rag/expand.hpp"

#include "agentty/io/http.hpp"

#include <algorithm>
#include <cctype>
#include <unordered_set>

#include <nlohmann/json.hpp>

namespace agentty::rag {

using json = nlohmann::json;

namespace {

// Strip a leading list marker ("1.", "-", "*", "•", "1)") and surrounding
// whitespace/quotes from one produced line, leaving the bare query text.
std::string clean_line(std::string s) {
    auto ltrim = [](std::string& x) {
        std::size_t i = 0;
        while (i < x.size() && std::isspace((unsigned char)x[i])) ++i;
        x.erase(0, i);
    };
    auto rtrim = [](std::string& x) {
        while (!x.empty() && std::isspace((unsigned char)x.back())) x.pop_back();
    };
    ltrim(s); rtrim(s);
    // Numbered / bulleted prefix.
    std::size_t i = 0;
    while (i < s.size() && std::isdigit((unsigned char)s[i])) ++i;
    if (i > 0 && i < s.size() && (s[i] == '.' || s[i] == ')')) {
        s.erase(0, i + 1);
        ltrim(s);
    } else if (!s.empty() && (s[0] == '-' || s[0] == '*' || s[0] == '+')) {
        s.erase(0, 1);
        ltrim(s);
    } else if (s.rfind("\xE2\x80\xA2", 0) == 0) {  // UTF-8 bullet •
        s.erase(0, 3);
        ltrim(s);
    }
    // Surrounding quotes.
    if (s.size() >= 2 && ((s.front() == '"' && s.back() == '"') ||
                          (s.front() == '\'' && s.back() == '\''))) {
        s = s.substr(1, s.size() - 2);
        ltrim(s); rtrim(s);
    }
    return s;
}

} // namespace

std::vector<std::string>
expand_query(const ExpandConfig& cfg, const std::string& query) {
    std::vector<std::string> out{query};   // original always first
    if (cfg.model.empty() || cfg.n == 0) return out;

    // Prompt the model for plain alternative phrasings, one per line. We do
    // NOT force a JSON schema here — line-per-query is the most robust shape
    // for weak models, and we parse defensively.
    std::string prompt =
        "You rewrite a search query into alternative phrasings to improve "
        "document retrieval. Given the user's query, output " +
        std::to_string(cfg.n) +
        " DIFFERENT search queries that capture the same intent using "
        "different wording, synonyms, and levels of specificity.\n"
        "Rules: output ONLY the queries, ONE per line, no numbering, no "
        "commentary, no blank lines.\n\n"
        "User query: " + query + "\n\nAlternative queries:";

    json body;
    body["model"]  = cfg.model;
    body["prompt"] = prompt;
    body["stream"] = false;
    // Keep it short + deterministic-ish; we just want phrasings.
    body["options"] = {{"temperature", 0.4}, {"num_predict", 256}};

    std::string body_str;
    try { body_str = body.dump(); } catch (...) { return out; }

    http::Request req;
    req.method    = http::HttpMethod::Post;
    req.host      = cfg.host;
    req.port      = cfg.port;
    req.path      = "/api/generate";
    req.plaintext = true;
    req.headers   = {{"content-type", "application/json"}};
    req.body      = std::move(body_str);
    req.max_body_bytes = 4ull * 1024 * 1024;

    http::Timeouts tos;
    tos.connect = std::chrono::milliseconds(3'000);
    tos.total   = std::chrono::milliseconds(30'000);

    auto resp = http::default_client().send(req, tos);
    if (!resp || resp->status != 200) return out;

    std::string text;
    try {
        auto j = json::parse(resp->body);
        if (j.contains("response") && j["response"].is_string())
            text = j["response"].get<std::string>();
    } catch (...) { return out; }
    if (text.empty()) return out;

    // Split into lines, clean each, dedupe (case-insensitive) against the
    // original + each other, cap at cfg.n variants.
    std::unordered_set<std::string> seen;
    auto norm = [](std::string s) {
        for (auto& c : s) c = static_cast<char>(std::tolower((unsigned char)c));
        return s;
    };
    seen.insert(norm(query));

    std::size_t start = 0;
    while (start <= text.size() && out.size() < cfg.n + 1) {
        std::size_t nl = text.find('\n', start);
        std::string line = text.substr(start,
            nl == std::string::npos ? std::string::npos : nl - start);
        start = (nl == std::string::npos) ? text.size() + 1 : nl + 1;

        std::string q = clean_line(std::move(line));
        if (q.size() < 2) continue;
        std::string key = norm(q);
        if (seen.insert(key).second) out.push_back(std::move(q));
    }

    return out;
}

std::string
hyde_document(const ExpandConfig& cfg, const std::string& query) {
    if (cfg.model.empty() || query.empty()) return {};

    // Ask for a short, direct passage that ANSWERS the query — the register of
    // a documentation paragraph, not a chat reply. "Be specific, use concrete
    // terms" steers the model toward the vocabulary real answer-passages use,
    // which is exactly what we want the embedding to sit near. Factual
    // accuracy is irrelevant; embedding-space proximity is the whole game.
    std::string prompt =
        "Write a short, factual passage (2–4 sentences) that directly answers "
        "the following question, as if excerpted from documentation or a "
        "reference article. Be specific and use concrete technical terms. "
        "Output ONLY the passage, no preamble.\n\nQuestion: " + query +
        "\n\nPassage:";

    json body;
    body["model"]  = cfg.model;
    body["prompt"] = prompt;
    body["stream"] = false;
    body["options"] = {{"temperature", 0.3}, {"num_predict", 220}};

    std::string body_str;
    try { body_str = body.dump(); } catch (...) { return {}; }

    http::Request req;
    req.method    = http::HttpMethod::Post;
    req.host      = cfg.host;
    req.port      = cfg.port;
    req.path      = "/api/generate";
    req.plaintext = true;
    req.headers   = {{"content-type", "application/json"}};
    req.body      = std::move(body_str);
    req.max_body_bytes = 4ull * 1024 * 1024;

    http::Timeouts tos;
    tos.connect = std::chrono::milliseconds(3'000);
    tos.total   = std::chrono::milliseconds(30'000);

    auto resp = http::default_client().send(req, tos);
    if (!resp || resp->status != 200) return {};

    std::string text;
    try {
        auto j = json::parse(resp->body);
        if (j.contains("response") && j["response"].is_string())
            text = j["response"].get<std::string>();
    } catch (...) { return {}; }

    // Trim surrounding whitespace; guard against a degenerate empty/echo reply.
    std::size_t a = 0, b = text.size();
    while (a < b && std::isspace((unsigned char)text[a])) ++a;
    while (b > a && std::isspace((unsigned char)text[b - 1])) --b;
    text = text.substr(a, b - a);
    if (text.size() < 8) return {};   // too short to be a useful hypothetical
    return text;
}

} // namespace agentty::rag
