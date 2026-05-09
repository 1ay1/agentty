#pragma once
// Composer attachments — the bytes that DON'T live inline in
// ComposerState::text. A long paste (or an @file pick) becomes an
// Attachment in the composer's `attachments` vector and a single
// placeholder token `\x01ATT:N\x01` is inserted at the cursor in
// `text`. The view renders the placeholder as a one-line chip; on
// submit the placeholder is substituted with the attachment's body
// so the model sees the literal bytes. Idea & UX directly from
// Cursor / Claude Code: tidy composer, full payload to the wire.
//
// Why a magic placeholder rather than a separate "rich text" model:
//   - Cursor / selection math stays plain-string. utf8_prev / utf8_next
//     just need to recognise the placeholder as a single navigation
//     unit.
//   - Multi-attachment ordering is whatever order they appear in
//     `text`; no parallel index to keep in sync.
//   - Submit-time substitution is a single string replace pass; no
//     special-case for "where does this attachment fit relative to
//     the prose around it."
//
// The placeholder bytes use \x01 (Start-Of-Heading, SOH) as the
// outer delimiter — a C0 control character that practically never
// appears in user-typed text or pastes (the StreamSink scrubs them
// to U+FFFD on the wire). The middle is `ATT:` + decimal index, so
// the whole token is `\x01ATT:0\x01` minimum (8 bytes), variable
// length thereafter.

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace moha {

struct Attachment {
    enum class Kind : std::uint8_t {
        Paste,      // Body is the verbatim pasted text.
        FileRef,    // Body is loaded from `path` at submit (or chip-build) time.
        Image,      // Body is the raw image bytes; `media_type` is set.
        Symbol,     // Body filled at submit from path:line span; carries `name`.
    };

    Kind kind = Kind::Paste;
    /// The bytes that get spliced into the message at submit. For a
    /// Paste this is filled at the moment of the paste; for a FileRef
    /// it's filled lazily on submit so the latest disk content is
    /// what reaches the model (but capped at a few hundred KiB).
    /// For Image it's the raw image bytes (NOT base64 — encoding is
    /// the transport's job so the in-memory chip stays compact).
    std::string body;
    /// Workspace-relative path for FileRef / Image / Symbol. Empty for Paste.
    std::string path;
    /// MIME type for Image attachments (e.g. "image/png", "image/jpeg").
    /// Empty for non-image kinds.
    std::string media_type;
    /// Symbol name for Symbol kind. Empty otherwise.
    std::string name;
    /// 1-based line number for Symbol kind. 0 otherwise.
    int         line_number = 0;
    /// Pre-computed line + byte counts for the chip caption. Avoids
    /// re-walking the body every frame the composer paints.
    std::size_t line_count = 0;
    std::size_t byte_count = 0;
};

namespace attachment {

// Placeholder boundary character. Pretty much never appears in
// real text — neither user-typed nor pasted. The outer wrapping is
// SOH-…-SOH; the middle is `ATT:` followed by the decimal index.
inline constexpr char kSentinel = '\x01';

/// Build the placeholder token for attachment index `idx`.
[[nodiscard]] std::string make_placeholder(std::size_t idx);

/// If text[pos] starts a placeholder, return the byte length of the
/// placeholder (so [pos, pos + len) is the full token). Returns 0
/// otherwise. The caller can compare against text.size() - pos for
/// safety; the function only inspects bytes that exist.
[[nodiscard]] std::size_t placeholder_len_at(std::string_view text,
                                             std::size_t pos) noexcept;

/// If text[pos - 1] is the closing sentinel of a placeholder, return
/// the byte length of the placeholder ending at pos. Returns 0
/// otherwise. Pairs with placeholder_len_at for cursor-walks both
/// directions.
[[nodiscard]] std::size_t placeholder_len_ending_at(std::string_view text,
                                                    std::size_t pos) noexcept;

/// Parse the index encoded inside a placeholder starting at `pos`.
/// Returns SIZE_MAX on malformed input (caller should treat as "no
/// attachment"). Use after placeholder_len_at confirms the shape.
[[nodiscard]] std::size_t placeholder_index(std::string_view text,
                                            std::size_t pos) noexcept;

/// Walk `text` and substitute each placeholder with the corresponding
/// attachment's body. Out-of-range indices are dropped silently
/// (defensive: a cursor edit that desynced the vector should not
/// surface garbage to the model). Adjacent runs of '\n' are
/// collapsed to a single paragraph break so multiple chips don't
/// produce four-blank-line gaps.
[[nodiscard]] std::string expand(std::string_view text,
                                 const std::vector<Attachment>& attachments);

/// Build the human-readable chip caption — e.g. "Pasted text · 412 lines · 14 KB"
/// or "@src/auth/login.cpp". The view uses this verbatim for inline render.
[[nodiscard]] std::string chip_label(const Attachment& a);

} // namespace attachment

} // namespace moha
