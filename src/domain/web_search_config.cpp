// web_search_config.cpp — web_search policy: pure request rewriting.

#include "agentty/domain/web_search_config.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <string>

namespace agentty::web_search_cfg {

namespace {

[[nodiscard]] std::string lower(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (unsigned char c : s) out.push_back(static_cast<char>(std::tolower(c)));
    return out;
}

// A host label set: ASCII letters, digits, '-' and '.', at least one dot,
// no empty label, no label that starts or ends with '-'. Non-ASCII is refused
// rather than guessed at — a punycode spelling ("xn--…") works, a raw IDN
// would go to the engine as bytes it may or may not fold the same way.
[[nodiscard]] bool plausible_host(std::string_view h) {
    if (h.empty() || h.size() > 253) return false;
    if (h.find('.') == std::string_view::npos) return false;
    std::size_t start = 0;
    while (start <= h.size()) {
        const auto dot = h.find('.', start);
        const auto label = h.substr(start, dot == std::string_view::npos
                                               ? std::string_view::npos
                                               : dot - start);
        if (label.empty() || label.size() > 63) return false;
        if (label.front() == '-' || label.back() == '-') return false;
        for (char ch : label) {
            const auto c = static_cast<unsigned char>(ch);
            if (!(std::isalnum(c) && c < 0x80) && ch != '-') return false;
        }
        if (dot == std::string_view::npos) break;
        start = dot + 1;
    }
    return true;
}

// "https://www.Example.com:8080/path?q" → "example.com". Everything a user
// might paste from an address bar reduces to the bare host the `site:`
// operator wants.
[[nodiscard]] std::string normalise(std::string_view raw) {
    std::string s = lower(raw);
    if (auto p = s.find("://"); p != std::string::npos) s.erase(0, p + 3);
    // A leading "-site:" / "site:" typed out of habit.
    for (std::string_view pfx : {"-site:", "site:"})
        if (s.starts_with(pfx)) { s.erase(0, pfx.size()); break; }
    if (auto p = s.find_first_of("/?#"); p != std::string::npos) s.resize(p);
    if (auto p = s.find('@'); p != std::string::npos) s.erase(0, p + 1);
    if (auto p = s.find(':'); p != std::string::npos) s.resize(p);
    while (!s.empty() && s.back() == '.') s.pop_back();
    if (s.starts_with("www.")) s.erase(0, 4);
    return s;
}

} // namespace

Mode mode(const Config& c) {
    if (c.mode_text == "on")  return Mode::On;
    if (c.mode_text == "off") return Mode::Off;
    return Mode::Auto;
}

std::vector<std::string> exclude_list(std::string_view text) {
    std::vector<std::string> out;
    std::size_t i = 0;
    while (i < text.size()) {
        while (i < text.size()
               && (text[i] == ' ' || text[i] == ',' || text[i] == '\t'
                   || text[i] == '\n' || text[i] == ';'))
            ++i;
        const std::size_t begin = i;
        while (i < text.size()
               && text[i] != ' ' && text[i] != ',' && text[i] != '\t'
               && text[i] != '\n' && text[i] != ';')
            ++i;
        if (i == begin) continue;
        std::string host = normalise(text.substr(begin, i - begin));
        if (!plausible_host(host)) continue;
        if (std::ranges::find(out, host) != out.end()) continue;
        out.push_back(std::move(host));
    }
    return out;
}

int effective_count(const Config& c, const nlohmann::json& args) {
    const int hi = std::max(kCountMin, c.max_count);
    int n = std::clamp(c.count, kCountMin, hi);
    if (args.is_object()) {
        if (auto it = args.find("count"); it != args.end()) {
            // Models send numbers as integers, floats and strings alike; take
            // any of them, and fall back to the default for anything else
            // (bool, null, object, "lots", "7abc") rather than letting a
            // malformed value reach mcp-cpp. Never throws: an unsigned value
            // past INT64_MAX or a huge float clamps instead.
            if (it->is_number_unsigned()) {
                const auto v = it->get<unsigned long long>();
                n = v > static_cast<unsigned long long>(hi)
                        ? hi
                        : std::max(kCountMin, static_cast<int>(v));
            } else if (it->is_number_integer()) {
                const auto v = it->get<long long>();
                n = static_cast<int>(std::clamp<long long>(v, kCountMin, hi));
            } else if (it->is_number_float()) {
                const double v = it->get<double>();
                if (std::isfinite(v))
                    n = static_cast<int>(std::clamp<double>(std::trunc(v), kCountMin, hi));
            } else if (it->is_string()) {
                const std::string& s = it->get_ref<const std::string&>();
                try {
                    std::size_t used = 0;
                    const long long v = std::stoll(s, &used);
                    // The whole string must be the number (surrounding
                    // spaces aside): "7abc" is not 7.
                    bool clean = true;
                    for (std::size_t i = used; i < s.size(); ++i)
                        if (!std::isspace(static_cast<unsigned char>(s[i]))) clean = false;
                    if (clean)
                        n = static_cast<int>(std::clamp<long long>(v, kCountMin, hi));
                } catch (...) {}
            }
        }
    }
    return n;
}

std::string apply_exclusions(std::string query,
                             const std::vector<std::string>& sites) {
    if (sites.empty()) return query;
    // Tokens the query already carries, lower-cased, so an exclusion the
    // model wrote itself is not appended a second time.
    std::vector<std::string> have;
    {
        const std::string q = lower(query);
        std::size_t i = 0;
        while (i < q.size()) {
            while (i < q.size() && std::isspace(static_cast<unsigned char>(q[i]))) ++i;
            const std::size_t b = i;
            while (i < q.size() && !std::isspace(static_cast<unsigned char>(q[i]))) ++i;
            if (i > b) have.emplace_back(q.substr(b, i - b));
        }
    }
    for (const auto& s : sites) {
        std::string op = "-site:" + s;
        if (std::ranges::find(have, op) != have.end()) continue;
        if (!query.empty() && !std::isspace(static_cast<unsigned char>(query.back())))
            query.push_back(' ');
        query += op;
        have.push_back(std::move(op));
    }
    return query;
}

nlohmann::json rewrite_args(const Config& c, nlohmann::json args) {
    if (!args.is_object()) return args;   // mcp-cpp reports the shape error
    // Auto leaves the count to the model, as before the pane existed. Only On
    // resolves and clamps it.
    if (mode(c) == Mode::On)
        args["count"] = effective_count(c, args);
    if (auto it = args.find("query"); it != args.end() && it->is_string()) {
        const auto sites = exclude_list(c.exclude_sites);
        if (!sites.empty())
            *it = apply_exclusions(it->get<std::string>(), sites);
    }
    return args;
}

bool dial_entry_ok(std::string_view key, std::string_view value) noexcept {
    if (key.empty() || key.size() > 64 || value.empty() || value.size() > 64) return false;
    const auto dot = key.find('.');
    if (dot == std::string_view::npos || dot == 0 || dot + 1 == key.size()) return false;
    auto printable = [](std::string_view s) {
        for (char ch : s) {
            const auto u = static_cast<unsigned char>(ch);
            if (u < 0x20 || u == 0x7f) return false;
        }
        return true;
    };
    return printable(key) && printable(value);
}

} // namespace agentty::web_search_cfg
