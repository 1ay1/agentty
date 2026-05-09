#pragma once
// Standard RFC 4648 base64 (NOT base64url): `+` and `/` for the
// 62/63 codes, `=` padding to a multiple of 4. Used at the
// Anthropic-API write boundary (image content blocks expect
// standard base64) and at the persistence-save boundary (so on-disk
// thread JSON can carry the raw image bytes losslessly).
//
// `auth.hpp` exports `base64url_no_pad` for the OAuth/PKCE flow —
// that uses the URL-safe alphabet (`-_` for 62/63) and no padding,
// which Anthropic's image API rejects. So we keep the two encoders
// distinct.

#include <cstddef>
#include <string>
#include <string_view>

namespace moha::util {

[[nodiscard]] std::string base64_encode(const unsigned char* data, std::size_t len);

[[nodiscard]] inline std::string base64_encode(std::string_view bytes) {
    return base64_encode(reinterpret_cast<const unsigned char*>(bytes.data()),
                         bytes.size());
}

/// Returns empty string on malformed input. Skips whitespace; tolerates
/// missing padding (rounds up to a multiple of 4).
[[nodiscard]] std::string base64_decode(std::string_view encoded);

} // namespace moha::util
