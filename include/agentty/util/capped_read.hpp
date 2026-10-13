#pragma once
// agentty::util::capped_read — reading a user file with a size limit, in a
// shape where ignoring "too big" does not compile.
//
// ── The bug this deletes ─────────────────────────────────────────────────
//
// Four files independently wrote the same helper:
//
//     std::string read_capped(const fs::path& p, std::size_t cap) {
//         if (!is_regular_file(p)) return {};
//         if (size > cap)          return {};     // <-- indistinguishable
//         ...
//     }
//
// and every one shipped the same bug, because an empty string answers three
// different questions at once: absent, unreadable, and too big to use.
// Callers wrote `if (raw.empty()) continue;` — right for the first two,
// silently wrong for the third. Measured, not guessed:
//
//   skills    an oversized SKILL.md vanished with `0 warning(s)`. The
//             author's skill did not exist and nothing said why.
//   hooks     `agentty hooks` printed "no hooks file" with the file right
//             there, so a PreToolUse hook written to BLOCK something was
//             inert while the user believed it was armed.
//   AGENTS.md cut at 64 KiB with no marker, so a rule near the bottom never
//             reached the model while the author watched the top of their
//             file take effect.
//
// Three separate finds, three separate fixes. That is the tell: a bug fixed
// three times in three files is one missing type.
//
// ── Why this is a variant and not an enum + string ───────────────────────
//
// jaal's first principle: "Contracts are types. If a rule can be a concept,
// a type-state or a private constructor, it is. Breaking it should fail to
// compile, with a message that names the rule."
//
// An enum beside a string is weaker than it looks — `c.text` is reachable
// whatever `c.kind` says, so the conflation is still WRITABLE and the rule
// survives only as long as reviewers keep noticing. A variant makes the
// outcomes alternatives: there is no `.text` to read until you have said
// which case you are in, and `visit` over a lambda set that omits one
// alternative is a compile error naming the type you forgot.
//
// So the four states are four types:
//
//     Content     the bytes, within the cap
//     Absent      nothing there. Normal.
//     Unreadable  present, but the read failed
//     TooBig      present, readable, over the limit — NEVER silent
//
// ── Why not std::expected<std::string, Error> ────────────────────────────
//
// TooBig is not "something went wrong" — the file is fine, agentty's limit
// is the constraint — and each caller answers it differently: skills keeps a
// reporting stub, hooks names the path and exits 1, AGENTS.md truncates WITH
// a marker rather than refusing. `expected` funnels all three into one
// `if (!r) return;`, which is exactly the collapse that caused the bug.

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <utility>
#include <variant>

namespace agentty::util {

// ── The four outcomes, as types ──────────────────────────────────────────

// Bytes, within the cap. An EMPTY file lands here, not in Absent: "the user
// wrote an empty hooks.json" and "there is no hooks.json" are different
// facts, and a caller that wants to treat them alike can say so.
struct Content { std::string text; };

// No such file, or not a regular file. The one case a caller may skip
// without comment.
//
// A DIRECTORY is Absent, deliberately. `std::ifstream` OPENS a directory
// successfully on Linux and throws out of the first read — that crashed this
// process once (`basic_filebuf::underflow error reading the file: Is a
// directory`, from `mkdir .agentty/mcp.json`). The is_regular_file check in
// front is what makes it impossible rather than merely unlikely.
struct Absent {};

// Present, but the open or the stat failed. Permissions, a race with a
// delete, a device that went away.
struct Unreadable {};

// Present and readable and OVER THE LIMIT. Carries the real size so the
// caller can say how far over, which is the difference between "your file is
// too big" and a number the user can act on.
struct TooBig { std::uintmax_t size = 0; };

// Exactly one of the four. No default-reachable `.text`, so a caller cannot
// read bytes without first saying which case they are in.
using Capped = std::variant<Content, Absent, Unreadable, TooBig>;

// ── Reading ──────────────────────────────────────────────────────────────

[[nodiscard]] inline Capped capped_read(const std::filesystem::path& p,
                                        std::size_t cap) {
    namespace fs = std::filesystem;
    std::error_code ec;
    if (p.empty() || !fs::is_regular_file(p, ec) || ec) return Absent{};

    const auto sz = fs::file_size(p, ec);
    if (ec)        return Unreadable{};
    if (sz == 0)   return Content{};
    if (sz > cap)  return TooBig{sz};

    std::ifstream f(p, std::ios::binary);
    if (!f) return Unreadable{};
    std::string buf(static_cast<std::size_t>(sz), '\0');
    f.read(buf.data(), static_cast<std::streamsize>(sz));
    buf.resize(static_cast<std::size_t>(f.gcount()));
    return Content{std::move(buf)};
}

// ── Reading the result ───────────────────────────────────────────────────
//
// `bytes_or_empty` is the ONE lossy accessor, and it is named so you cannot
// reach for it by accident. It exists for the handful of call sites that
// genuinely treat every failure alike (a probe asking only "is there
// anything here"). Everywhere else, visit — the compiler will not let you
// forget TooBig.
[[nodiscard]] inline const std::string& bytes_or_empty(const Capped& c) {
    static const std::string kEmpty;
    const auto* got = std::get_if<Content>(&c);
    return got ? got->text : kEmpty;
}

[[nodiscard]] inline bool has_content(const Capped& c) noexcept {
    return std::holds_alternative<Content>(c);
}

// The real size when over the cap, else 0. For building the message.
[[nodiscard]] inline std::uintmax_t oversize_bytes(const Capped& c) noexcept {
    const auto* big = std::get_if<TooBig>(&c);
    return big ? big->size : 0;
}

// A one-line explanation of a non-Content result, for a CLI row or a lint
// warning. Empty for Content and Absent — neither needs saying.
[[nodiscard]] inline std::string capped_note(const Capped& c,
                                             std::size_t cap) {
    return std::visit([cap](const auto& v) -> std::string {
        using V = std::decay_t<decltype(v)>;
        if constexpr (std::is_same_v<V, TooBig>) {
            return "is " + std::to_string(v.size / 1024) + " KB — over the "
                 + std::to_string(cap / 1024)
                 + " KB limit, so it was NOT loaded";
        } else if constexpr (std::is_same_v<V, Unreadable>) {
            return "could not be read";
        } else {
            return {};
        }
    }, c);
}

}  // namespace agentty::util
