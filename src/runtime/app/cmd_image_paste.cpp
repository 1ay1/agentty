// cmd::sniff_image_path — is this pasted path an image file?
//
// A dropped file arrives as its path in a bracketed paste. Deciding whether
// it names an image means stat-ing and reading the file, so it runs on a
// worker and replies with ImagePathSniffed. The reducer attaches the image,
// or pastes the text as text when the path turns out not to be one.

#include "agentty/runtime/app/cmd_factory.hpp"

#include <cctype>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>

#include "agentty/util/home_dir.hpp"

namespace agentty::app::detail {
const char* detect_image_media_type(std::string_view bytes) noexcept;
}

namespace agentty::app::cmd {

namespace {

namespace fs = std::filesystem;
using detail::detect_image_media_type;

// Returns (path, media_type) if the paste looks like a single-line
// path to a recognised image file. Empty path on no match.
struct ImagePasteResult {
    std::string  path;
    const char*  media_type = nullptr;
    std::string  body;       // raw image bytes
};

// Normalise a pasted path candidate:
//   – expand a leading `~/` to $HOME (file managers / shells emit this)
//   – unescape `\ ` / `\(` / `\)` / `\'` etc. (drag-drop on macOS and
//     several Linux file managers backslash-escape every shell-special
//     character so the path can be re-pasted into a shell verbatim)
//   – drop CR if a CRLF terminal slipped one through
std::string normalize_path_candidate(std::string_view in) {
    // Trim trailing CR.
    while (!in.empty() && (in.back() == '\r' || in.back() == ' ')) in.remove_suffix(1);
    std::string s;
    s.reserve(in.size());
    if (in.size() >= 2 && in[0] == '~' && in[1] == '/') {
        // Unified home root ($HOME on POSIX/MSYS2, $USERPROFILE on native
        // Windows) so a dropped `~/…` path expands on every platform.
        const fs::path home = agentty::util::home_dir();
        if (!home.empty()) {
            s.append(home.string());
            in.remove_prefix(1);  // keep the '/'
        }
    }
#ifdef _WIN32
    // On Windows the backslash is the PATH SEPARATOR, not a shell escape:
    // a dropped native path `C:\Users\foo\bar` must survive verbatim.
    // Stripping `\` here would mangle it to `C:Usersfoobar`. Take the
    // whole remainder as-is (drag/drop sources on Windows don't
    // shell-escape).
    s.append(in);
#else
    for (std::size_t i = 0; i < in.size(); ++i) {
        if (in[i] == '\\' && i + 1 < in.size()) {
            // Shell-style escape (macOS / Linux file managers backslash
            // every shell-special char). Drop the backslash, keep the char.
            s.push_back(in[i + 1]);
            ++i;
            continue;
        }
        s.push_back(in[i]);
    }
#endif
    return s;
}

ImagePasteResult sniff_image_paste(std::string_view text) {
    ImagePasteResult r;
    // Trim leading / trailing whitespace (incl. CR).
    std::size_t a = 0, b = text.size();
    while (a < b && std::isspace(static_cast<unsigned char>(text[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(text[b - 1]))) --b;
    auto trimmed = text.substr(a, b - a);
    if (trimmed.empty()) return r;
    // Must be a single line — embedded newlines hint at pasted prose
    // rather than a path.
    if (trimmed.find('\n') != std::string_view::npos) return r;
    if (trimmed.size() > 4096) return r;  // sane upper bound

    // Strip surrounding quotes (drag-and-drop on macOS / GNOME quotes
    // paths automatically).
    if (trimmed.size() >= 2
        && (trimmed.front() == '\'' || trimmed.front() == '"')
        && trimmed.back() == trimmed.front()) {
        trimmed = trimmed.substr(1, trimmed.size() - 2);
    }
    // file:// URIs land here on some desktops. Accept either two or
    // three leading slashes (file:// vs file:///).
    constexpr std::string_view kFileUri = "file://";
    if (trimmed.size() > kFileUri.size()
        && trimmed.substr(0, kFileUri.size()) == kFileUri) {
        trimmed.remove_prefix(kFileUri.size());
        // Some emitters use file:/// for absolute paths. Collapse the
        // extra slash so the result starts with exactly one '/'.
        if (!trimmed.empty() && trimmed.front() == '/') {
            // already absolute, fine
        }
    }

    auto candidate = normalize_path_candidate(trimmed);
    if (candidate.empty()) return r;

    fs::path p{candidate};
    std::error_code ec;
    if (!fs::is_regular_file(p, ec) || ec) return r;
    // Sniff first 16 bytes for a magic prefix.
    std::ifstream in(p, std::ios::binary);
    if (!in) return r;
    char buf[16]{};
    in.read(buf, sizeof(buf));
    auto got = static_cast<std::size_t>(in.gcount());
    auto* mt = detect_image_media_type(std::string_view{buf, got});
    if (!mt) return r;
    // Slurp full bytes. 8 MiB cap — Anthropic's per-image limit is
    // 5 MB and base64 expansion adds ~33 %, so anything bigger than
    // ~6 MB on disk would fail server-side anyway.
    auto sz = fs::file_size(p, ec);
    if (ec || sz > 8 * 1024 * 1024) return r;
    in.clear();
    in.seekg(0);
    std::string body(static_cast<std::size_t>(sz), '\0');
    in.read(body.data(), static_cast<std::streamsize>(sz));
    if (in.gcount() != static_cast<std::streamsize>(sz)) return r;

    r.path       = p.string();
    r.media_type = mt;
    r.body       = std::move(body);
    return r;
}

}  // namespace

Cmd sniff_image_path(std::string text) {
    return Cmd::task(
        [](maya::Sink<Msg> out, std::stop_token, std::string text) {
            ImagePathSniffed r;
            try {
                auto img = sniff_image_paste(text);
                if (!img.path.empty() && img.media_type) {
                    r.path       = std::move(img.path);
                    r.media_type = img.media_type;
                    r.body       = std::move(img.body);
                }
            } catch (...) {
                r = ImagePathSniffed{};
            }
            r.text = std::move(text);
            out.send(Msg{msg::ComposerMsg{std::move(r)}});
        },
        std::move(text));
}

}  // namespace agentty::app::cmd
