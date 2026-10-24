#include "agentty/tool/util/partial_json.hpp"

#include <cctype>
#include <cstring>
#include <vector>

namespace agentty::tools::util {

// A small JSON walker that knows where in the grammar it is, so a cut can be
// closed into valid JSON whatever it lands on: half a key (`{"co`), a key
// without its colon (`{"a"`), half a literal (`tr`, `nul`), half a number
// (`-`, `1.`, `1e`), a dangling comma, or an open string. The rule is to keep
// every complete member and drop the one being written, except a value string
// cut mid-text, which keeps its text so the live preview can show it.
std::string close_partial_json(std::string_view raw) {
    // What a container is waiting for next.
    enum class Want : char {
        KeyOrEnd,    // `{` just opened: a key or `}`
        Key,         // after `,` in an object
        Colon,       // after a key
        Value,       // after `:` or `,` in an array / `[` (or `]`)
        CommaOrEnd,  // a member is complete
    };
    struct Frame { char open; Want want; };
    std::vector<Frame> stack;
    // Byte offset in `out` to cut back to when the member being written is
    // dropped: just before its key (objects) or its leading comma (arrays).
    std::vector<std::size_t> member_start;

    std::string out;
    out.reserve(raw.size() + 16);
    bool top_done = false;      // a whole top-level value has been written

    // An open token at EOF: a string (key or value), or a bare literal/number.
    enum class Tok { None, KeyString, ValueString, Bare };
    Tok tok = Tok::None;
    bool escape = false;
    std::size_t bare_from = 0;  // where the current bare token started in out

    auto in_array = [&] { return !stack.empty() && stack.back().open == '['; };
    auto value_done = [&] {
        if (stack.empty()) top_done = true;
        else stack.back().want = Want::CommaOrEnd;
    };
    auto end_bare = [&] {
        if (tok == Tok::Bare) { tok = Tok::None; value_done(); }
    };

    for (char c : raw) {
        if (tok == Tok::KeyString || tok == Tok::ValueString) {
            out.push_back(c);
            if (escape) { escape = false; continue; }
            if (c == '\\') { escape = true; continue; }
            if (c == '"') {
                if (tok == Tok::KeyString) stack.back().want = Want::Colon;
                else value_done();
                tok = Tok::None;
            }
            continue;
        }
        if (tok == Tok::Bare) {
            const bool part = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z')
                           || c == '-' || c == '+' || c == '.' || c == 'E';
            if (part) { out.push_back(c); continue; }
            end_bare();
        }
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') { out.push_back(c); continue; }

        const Want want = stack.empty() ? Want::Value : stack.back().want;
        switch (c) {
            case '{': case '[':
                if (want == Want::Value || want == Want::KeyOrEnd) {
                    if (in_array() && want == Want::Value && member_start.size() < stack.size())
                        member_start.push_back(out.size());
                    out.push_back(c);
                    stack.push_back({c, c == '{' ? Want::KeyOrEnd : Want::Value});
                }
                break;
            case '}': case ']':
                if (!stack.empty() && stack.back().open == (c == '}' ? '{' : '[')) {
                    out.push_back(c);
                    stack.pop_back();
                    if (member_start.size() > stack.size()) member_start.resize(stack.size());
                    value_done();
                }
                break;
            case ':':
                if (want == Want::Colon) { out.push_back(c); stack.back().want = Want::Value; }
                break;
            case ',':
                if (want == Want::CommaOrEnd) {
                    // the member before this comma is complete; a new one starts
                    if (member_start.size() == stack.size()) member_start.back() = out.size();
                    else member_start.push_back(out.size());
                    out.push_back(c);
                    stack.back().want = stack.back().open == '{' ? Want::Key : Want::Value;
                }
                break;
            case '"':
                if (want == Want::KeyOrEnd || want == Want::Key) {
                    if (member_start.size() < stack.size()) member_start.push_back(out.size());
                    tok = Tok::KeyString;
                } else {
                    if (in_array() && member_start.size() < stack.size())
                        member_start.push_back(out.size());
                    tok = Tok::ValueString;
                }
                out.push_back(c);
                break;
            default:
                if (want == Want::Value) {
                    if (in_array() && member_start.size() < stack.size())
                        member_start.push_back(out.size());
                    bare_from = out.size();
                    tok = Tok::Bare;
                    out.push_back(c);
                }
                break;   // anything else is noise we can't place; drop it
        }
    }

    // ── Close what is open at EOF ──────────────────────────────────────
    // A bare token is kept only if it is already a whole literal or number.
    if (tok == Tok::Bare) {
        const std::string_view t{out.data() + bare_from, out.size() - bare_from};
        const bool literal = t == "true" || t == "false" || t == "null";
        const bool number = !t.empty()
            && (std::isdigit(static_cast<unsigned char>(t.back())) != 0)
            && t.find_first_not_of("0123456789+-.eE") == std::string_view::npos;
        if (literal || number) {
            value_done();
        } else {
            out.resize(bare_from);   // `tr`, `-`, `1.`: not a value yet
        }
        tok = Tok::None;
    }
    if (tok == Tok::ValueString) {
        if (escape) out.pop_back();          // a lone `\` at the cut
        // the cut can fall inside a multi-byte character (`é`, `日`); its
        // leading bytes alone are not valid UTF-8, so drop them
        {
            std::size_t k = out.size(), cont = 0;
            while (k > 0 && cont < 3
                   && (static_cast<unsigned char>(out[k - 1]) & 0xC0) == 0x80) { --k; ++cont; }
            if (k > 0) {
                const auto lead = static_cast<unsigned char>(out[k - 1]);
                const std::size_t need = lead >= 0xF0 ? 3 : lead >= 0xE0 ? 2 : lead >= 0xC0 ? 1 : 0;
                if (need > cont) out.resize(k - 1);
            }
        }
        // a cut inside \uXXXX leaves an invalid escape: drop it
        if (const auto u = out.rfind("\\u"); u != std::string::npos
            && out.size() - u < 6
            && out.find('"', u) == std::string::npos) {
            std::size_t bs = 0;
            for (std::size_t k = u; k > 0 && out[k - 1] == '\\'; --k) ++bs;
            if (bs % 2 == 0) out.resize(u);   // the `\` really starts an escape
        }
        // a high surrogate (\uD800-\uDBFF) needs its low half after it; a cut
        // between the two leaves an escape no parser accepts, so drop it
        if (out.size() >= 6 && out[out.size() - 6] == '\\' && out[out.size() - 5] == 'u') {
            std::size_t bs = 0;
            for (std::size_t k = out.size() - 6; k > 0 && out[k - 1] == '\\'; --k) ++bs;
            const char h0 = static_cast<char>(std::tolower(static_cast<unsigned char>(out[out.size() - 4])));
            const char h1 = static_cast<char>(std::tolower(static_cast<unsigned char>(out[out.size() - 3])));
            if (bs % 2 == 0 && h0 == 'd' && h1 >= '8' && h1 <= 'b') out.resize(out.size() - 6);
        }
        out.push_back('"');
        value_done();
    }
    // A key cut mid-string leaves its frame wanting a key, but the key's
    // bytes are in `out`. Mark it unfinished so the cut below removes it.
    const bool key_open = tok == Tok::KeyString;
    tok = Tok::None;

    bool innermost = true;
    while (!stack.empty()) {
        Frame& f = stack.back();
        const bool has_partial_member = member_start.size() == stack.size();
        // Unfinished = anything but a complete member or an empty container.
        // Only the innermost frame can be mid-member; outer frames are
        // mid-value, and that value is the inner container being closed.
        const bool unfinished = innermost
            && (key_open || (f.want != Want::CommaOrEnd && f.want != Want::KeyOrEnd));
        if (unfinished && has_partial_member) out.resize(member_start.back());
        innermost = false;
        // trim trailing whitespace and a now-dangling comma
        while (!out.empty() && (out.back() == ' ' || out.back() == '\t'
                                || out.back() == '\n' || out.back() == '\r'))
            out.pop_back();
        if (!out.empty() && out.back() == ',') out.pop_back();
        out.push_back(f.open == '{' ? '}' : ']');
        stack.pop_back();
        if (member_start.size() > stack.size()) member_start.resize(stack.size());
    }

    if (!top_done && out.find_first_not_of(" \t\r\n") == std::string::npos) return "null";
    return out;
}

namespace {

// Shared prefix walk for sniff_*: advance past `"key"` followed by `:` and
// the opening `"` of its value. Returns the index just past the opening
// quote, or std::string_view::npos if the expected prefix isn't there yet.
//
// Perf note: this runs on the streaming hot path (called per alias per
// preview tick, ~8x/sec during a turn). The previous implementation built
// a `"key"` needle as a heap-allocated std::string and handed it to
// std::string_view::find — O(N) scan per call, plus a malloc+free every
// call. We now scan the buffer once looking for the opening `"`, then
// memcmp against the key. Zero allocations; the inner loop is a single
// byte compare that usually fails on the first char.
std::size_t sniff_prefix(std::string_view raw, std::string_view key) {
    if (key.empty() || raw.size() < key.size() + 4) return std::string_view::npos;
    const char* const base = raw.data();
    const std::size_t n    = raw.size();
    const std::size_t klen = key.size();
    // Last index where `"key"` could still begin: need klen+2 bytes
    // for the quoted key plus at least `:` `"` after it.
    const std::size_t last_start = n - (klen + 3);
    for (std::size_t i = 0; i <= last_start; ++i) {
        if (base[i] != '"') continue;
        if (base[i + klen + 1] != '"') continue;
        if (std::memcmp(base + i + 1, key.data(), klen) != 0) continue;
        std::size_t p = i + klen + 2;
        while (p < n && (base[p] == ' ' || base[p] == '\t' || base[p] == '\n')) ++p;
        if (p >= n || base[p] != ':') continue;
        ++p;
        while (p < n && (base[p] == ' ' || base[p] == '\t' || base[p] == '\n')) ++p;
        if (p >= n || base[p] != '"') continue;
        return p + 1;
    }
    return std::string_view::npos;
}

// Apply a JSON escape (`\X`) onto the output buffer. Unknown escapes
// degrade to the literal byte — fine for streaming previews, where we
// prefer a benign display over an exception.
void apply_escape(char n, std::string& out) {
    switch (n) {
        case 'n':  out += '\n'; break;
        case 't':  out += '\t'; break;
        case 'r':  out += '\r'; break;
        case '"':  out += '"';  break;
        case '\\': out += '\\'; break;
        case '/':  out += '/';  break;
        case 'b':  out += '\b'; break;
        case 'f':  out += '\f'; break;
        default:   out += n;    break;
    }
}

} // namespace

std::optional<std::string>
sniff_string(std::string_view raw, std::string_view key) {
    std::size_t p = sniff_prefix(raw, key);
    if (p == std::string_view::npos) return std::nullopt;
    std::string out;
    out.reserve(64);
    while (p < raw.size()) {
        char c = raw[p];
        if (c == '\\') {
            if (p + 1 >= raw.size()) return std::nullopt; // wait for next delta
            apply_escape(raw[p + 1], out);
            p += 2;
        } else if (c == '"') {
            return out;
        } else {
            out.push_back(c);
            ++p;
        }
    }
    return std::nullopt;
}

std::optional<std::size_t>
locate_string_value(std::string_view raw, std::string_view key) {
    std::size_t p = sniff_prefix(raw, key);
    if (p == std::string_view::npos) return std::nullopt;
    return p;
}

std::string
decode_string_from(std::string_view raw, std::size_t offset) {
    std::string out;
    if (offset >= raw.size()) return out;
    out.reserve(raw.size() - offset);
    std::size_t p = offset;
    while (p < raw.size()) {
        char c = raw[p];
        if (c == '\\') {
            if (p + 1 >= raw.size()) break; // half-escape at buffer edge
            apply_escape(raw[p + 1], out);
            p += 2;
        } else if (c == '"') {
            return out;
        } else {
            out.push_back(c);
            ++p;
        }
    }
    return out;
}

bool
decode_string_append(std::string_view raw,
                     std::size_t* through,
                     std::string& out,
                     bool* closed)
{
    // Once the closing `"` was seen, later calls must not decode whatever
    // follows it (the next JSON member) into the value.
    if (closed && *closed) return true;
    if (!through || *through >= raw.size()) return false;
    std::size_t p = *through;
    // Reserve a guess for the new bytes — most escapes are 1-byte
    // expansions (`\n` → newline) so raw.size() - p is a tight upper
    // bound on the decoded growth.
    if (out.capacity() - out.size() < raw.size() - p)
        out.reserve(out.size() + (raw.size() - p));
    while (p < raw.size()) {
        char c = raw[p];
        if (c == '\\') {
            // Half-escape at the buffer edge — stop without consuming
            // the `\`. Next call (with more bytes) will see the full
            // escape pair and resume cleanly.
            if (p + 1 >= raw.size()) break;
            apply_escape(raw[p + 1], out);
            p += 2;
        } else if (c == '"') {
            // Value closed. Advance past the `"` so future calls
            // return immediately (idempotent post-close).
            ++p;
            *through = p;
            if (closed) *closed = true;
            return true;
        } else {
            out.push_back(c);
            ++p;
        }
    }
    *through = p;
    return false;
}

std::optional<std::string>
sniff_string_progressive(std::string_view raw, std::string_view key) {
    auto off = locate_string_value(raw, key);
    if (!off) return std::nullopt;
    return decode_string_from(raw, *off);
}

bool
ended_inside_string(std::string_view raw) noexcept {
    // Same state machine as close_partial_json's main loop, minus the
    // emit buffer. We only need the final `in_string` flag.
    bool in_string = false;
    bool escape    = false;
    for (char c : raw) {
        if (in_string) {
            if (escape)         { escape = false; continue; }
            if (c == '\\')      { escape = true;  continue; }
            if (c == '"')       { in_string = false; continue; }
            continue;
        }
        if (c == '"') in_string = true;
    }
    // A trailing lone `\` at the very edge of a string is also a
    // half-finished value (close_partial_json drops it before
    // synthesising the `"`); treat it as "ended inside" so callers
    // refuse the salvage just the same.
    return in_string;
}

} // namespace agentty::tools::util
