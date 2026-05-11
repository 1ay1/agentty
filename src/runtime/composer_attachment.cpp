#include "agentty/runtime/composer_attachment.hpp"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <sstream>

#include "agentty/tool/util/fs_helpers.hpp"

namespace agentty::attachment {

std::string make_placeholder(std::size_t idx) {
    // \x01ATT:N\x01 — see header for layout rationale.
    std::string out;
    out.reserve(8);
    out.push_back(kSentinel);
    out.append("ATT:");
    out.append(std::to_string(idx));
    out.push_back(kSentinel);
    return out;
}

std::size_t placeholder_len_at(std::string_view text, std::size_t pos) noexcept {
    // Shape: \x01 A T T : <digits> \x01
    // Minimum length 7 (one digit). Bail early on the cheap mismatches.
    if (pos >= text.size() || text[pos] != kSentinel) return 0;
    if (pos + 7 > text.size()) return 0;
    if (text[pos + 1] != 'A' || text[pos + 2] != 'T'
        || text[pos + 3] != 'T' || text[pos + 4] != ':') return 0;
    // Walk digits.
    std::size_t p = pos + 5;
    if (p >= text.size() || text[p] < '0' || text[p] > '9') return 0;
    while (p < text.size() && text[p] >= '0' && text[p] <= '9') ++p;
    if (p >= text.size() || text[p] != kSentinel) return 0;
    return (p + 1) - pos;
}

std::size_t placeholder_len_ending_at(std::string_view text, std::size_t pos) noexcept {
    // pos is the byte AFTER the closing sentinel; placeholder occupies
    // [start, pos). Walk backwards to find the opening \x01.
    if (pos == 0) return 0;
    if (text[pos - 1] != kSentinel) return 0;
    // The token ends with: <digits>\x01. Step back past digits.
    if (pos < 2) return 0;
    std::size_t p = pos - 2;
    while (p > 0 && text[p] >= '0' && text[p] <= '9') --p;
    // Now p should point at ':' of "ATT:".
    if (text[p] != ':') return 0;
    // Need exactly the prefix \x01ATT.
    if (p < 4) return 0;
    if (text[p - 1] != 'T' || text[p - 2] != 'T' || text[p - 3] != 'A'
        || text[p - 4] != kSentinel) return 0;
    std::size_t start = p - 4;
    // Confirm forward shape (digits + closing sentinel) since the
    // backward walk above already established the suffix.
    auto fwd = placeholder_len_at(text, start);
    if (fwd != pos - start) return 0;
    return fwd;
}

std::size_t placeholder_index(std::string_view text, std::size_t pos) noexcept {
    auto len = placeholder_len_at(text, pos);
    if (len == 0) return static_cast<std::size_t>(-1);
    // Digits live at [pos+5, pos+len-1).
    std::size_t value = 0;
    for (std::size_t p = pos + 5; p + 1 < pos + len; ++p) {
        value = value * 10 + static_cast<std::size_t>(text[p] - '0');
    }
    return value;
}

namespace {

// Truncation cap on a single FileRef expansion. 256 KiB covers
// essentially every real source file while preventing a careless @ on
// a build artefact / log / asset from drowning the prompt.
inline constexpr std::size_t kFileRefMaxBytes = 256 * 1024;

std::string render_attachment_body(const Attachment& a) {
    if (a.kind == Attachment::Kind::FileRef) {
        // Lazy body load. Caller may have pre-filled `body` (test
        // harness, paste-of-a-file path, etc.); honour that. Otherwise
        // resolve the path against the workspace root and slurp it.
        std::string body = a.body;
        if (body.empty() && !a.path.empty()) {
            namespace fs = std::filesystem;
            fs::path p{a.path};
            if (p.is_relative()) p = tools::util::workspace_root() / p;
            body = tools::util::read_file(p);
        }
        std::string out;
        out.reserve(body.size() + a.path.size() + 32);
        // Path header keeps the model anchored to which file the
        // following block came from — same pattern Cursor / Claude
        // Code use for inline file references.
        out.append("// path: ");
        out.append(a.path);
        out.push_back('\n');
        if (body.size() > kFileRefMaxBytes) {
            out.append(body.data(), kFileRefMaxBytes);
            out.append("\n[… file truncated, ");
            out.append(std::to_string(body.size() - kFileRefMaxBytes));
            out.append(" bytes elided]");
        } else {
            out.append(body);
        }
        return out;
    }
    if (a.kind == Attachment::Kind::Symbol) {
        // Excerpt 20 lines centred on the symbol's declaration line so
        // the model gets immediate signature context plus a few lines
        // of body without loading the whole file. The caller (transport
        // / submit-time wire-up) sees this as a code-fenced excerpt
        // with a `// symbol:` header so it's clearly anchored.
        constexpr int kSymbolWindow = 20;
        std::string body = a.body;
        if (body.empty() && !a.path.empty()) {
            namespace fs = std::filesystem;
            fs::path p{a.path};
            if (p.is_relative()) p = tools::util::workspace_root() / p;
            body = tools::util::read_file(p);
        }
        std::string out;
        out.reserve(a.name.size() + a.path.size() + 64);
        out.append("// symbol: ");
        out.append(a.name);
        out.append(" (");
        out.append(a.path);
        out.push_back(':');
        out.append(std::to_string(a.line_number));
        out.append(")\n");
        // Walk the body, count lines, splice the window.
        int target = a.line_number > 0 ? a.line_number : 1;
        int start = std::max(1, target - kSymbolWindow / 4);
        int end   = target + (kSymbolWindow * 3 / 4);
        int line  = 1;
        std::size_t i = 0;
        while (i < body.size() && line < start) {
            if (body[i] == '\n') ++line;
            ++i;
        }
        while (i < body.size() && line <= end) {
            out.push_back(body[i]);
            if (body[i] == '\n') ++line;
            ++i;
        }
        if (out.empty() || out.back() != '\n') out.push_back('\n');
        return out;
    }
    if (a.kind == Attachment::Kind::Image) {
        // Transport-level wiring (Anthropic image content blocks) is
        // handled outside this text-flatten path — the messages-API
        // builder pulls Image attachments directly off the composer
        // before the prose is serialised. We still emit a textual
        // marker so the prose around the chip stays anchored to where
        // the user dropped the image, and so debugging logs (which
        // print the flattened user.text) name the file.
        std::string out;
        out.reserve(a.path.size() + 24);
        out.append("[image: ");
        out.append(a.path.empty() ? "<inline>" : a.path);
        out.push_back(']');
        return out;
    }
    return a.body;
}

} // namespace

std::string expand(std::string_view text,
                   const std::vector<Attachment>& attachments) {
    std::string out;
    out.reserve(text.size() + attachments.size() * 64);
    std::size_t i = 0;
    while (i < text.size()) {
        if (text[i] == kSentinel) {
            auto len = placeholder_len_at(text, i);
            if (len > 0) {
                auto idx = placeholder_index(text, i);
                if (idx < attachments.size()) {
                    // Wrap the spliced body in paragraph breaks so
                    // surrounding prose doesn't run into it. Adjacent
                    // chips collapse to a single \n\n via the
                    // post-pass below.
                    if (!out.empty() && out.back() != '\n') out.push_back('\n');
                    if (!out.empty() && (out.size() < 2 || out[out.size() - 2] != '\n'))
                        out.push_back('\n');
                    out.append(render_attachment_body(attachments[idx]));
                    // Ensure the body ends on its own line so following
                    // content doesn't run into it.
                    if (out.empty() || out.back() != '\n') out.push_back('\n');
                }
                i += len;
                // Add a blank-line separator ONLY when more content
                // (text or another chip) follows this placeholder.
                // Without this guard a paste-as-only-message ends with
                // a trailing \n\n that renders as ghost blank rows
                // below the body — visible as `┃` / `┃` rows at the
                // bottom of the user-turn box.
                if (i < text.size()) out.push_back('\n');
                continue;
            }
            // Stray sentinel byte (corrupted or user-typed control
            // char). Drop silently — the message body shouldn't carry
            // C0 control bytes anyway.
            ++i;
            continue;
        }
        out.push_back(text[i++]);
    }
    // Collapse runs of 3+ '\n' to exactly two so back-to-back chips
    // don't produce visible blank-paragraph stacks.
    std::string collapsed;
    collapsed.reserve(out.size());
    int run = 0;
    for (char c : out) {
        if (c == '\n') {
            if (++run <= 2) collapsed.push_back('\n');
        } else {
            run = 0;
            collapsed.push_back(c);
        }
    }
    return collapsed;
}

namespace {

// "src/runtime/foo.cpp" → "foo.cpp". The chip caption in the inline
// composer is space-constrained — full paths take 30-70 cols and
// dominate the visible row. The user just picked the file from the
// @-picker (where they saw the full path), so the filename alone is
// enough to remind them which one's attached. Submit-time expansion
// still uses the full path.
std::string filename_only(const std::string& path) {
    auto slash = path.find_last_of('/');
    if (slash == std::string::npos) return path;
    return path.substr(slash + 1);
}

} // namespace

std::string chip_label(const Attachment& a) {
    char buf[256];
    if (a.kind == Attachment::Kind::FileRef) {
        std::snprintf(buf, sizeof(buf), "@%s", filename_only(a.path).c_str());
        return buf;
    }
    if (a.kind == Attachment::Kind::Symbol) {
        std::snprintf(buf, sizeof(buf), "#%s \xc2\xb7 %s:%d",
                      a.name.c_str(),
                      filename_only(a.path).c_str(), a.line_number);
        return buf;
    }
    if (a.kind == Attachment::Kind::Image) {
        const char* type = a.media_type.empty() ? "image" : a.media_type.c_str();
        // <clipboard> / <paste> get their pseudo-name through; a
        // real path gets just the filename for the same compactness
        // reason as FileRef.
        bool sentinel = !a.path.empty() && a.path.front() == '<';
        std::string where = sentinel ? a.path : filename_only(a.path);
        if (a.byte_count >= 1024) {
            std::snprintf(buf, sizeof(buf),
                          "Image \xc2\xb7 %s \xc2\xb7 %s \xc2\xb7 %zu KB",
                          where.c_str(), type, a.byte_count / 1024);
        } else {
            std::snprintf(buf, sizeof(buf),
                          "Image \xc2\xb7 %s \xc2\xb7 %s \xc2\xb7 %zu B",
                          where.c_str(), type, a.byte_count);
        }
        return buf;
    }
    // Paste:
    //   – Single-line content → caption shows the actual text (up to
    //     ~50 cols) so the user can see what they pasted without
    //     having to undo and retype. Newlines / tabs collapse to
    //     spaces for readability.
    //   – Multi-line content → "lines · bytes" summary; the body is
    //     too big to preview usefully and the caption stays tight.
    if (a.line_count <= 1) {
        std::string preview;
        preview.reserve(60);
        constexpr std::size_t kPreviewMax = 50;
        bool truncated = false;
        for (char c : a.body) {
            if (preview.size() >= kPreviewMax) { truncated = true; break; }
            if (c == '\n' || c == '\t' || c == '\r') {
                if (preview.empty() || preview.back() != ' ') preview.push_back(' ');
            } else if (static_cast<unsigned char>(c) >= 0x20) {
                preview.push_back(c);
            }
        }
        while (!preview.empty() && preview.back() == ' ') preview.pop_back();
        if (truncated) preview.append("\xe2\x80\xa6");  // …
        if (preview.empty()) {
            std::snprintf(buf, sizeof(buf),
                          "Pasted text \xc2\xb7 %zu B", a.byte_count);
        } else {
            std::snprintf(buf, sizeof(buf), "Pasted: %s", preview.c_str());
        }
        return buf;
    }
    if (a.byte_count >= 1024) {
        std::snprintf(buf, sizeof(buf),
                      "Pasted text \xc2\xb7 %zu lines \xc2\xb7 %zu KB",
                      a.line_count, a.byte_count / 1024);
    } else {
        std::snprintf(buf, sizeof(buf),
                      "Pasted text \xc2\xb7 %zu lines \xc2\xb7 %zu B",
                      a.line_count, a.byte_count);
    }
    return buf;
}

} // namespace agentty::attachment
