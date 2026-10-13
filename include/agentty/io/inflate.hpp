#pragma once
// agentty::io — HTTP content-encoding decoding (gzip / zlib / raw DEFLATE).
//
// agentty's HTTP client asks servers for `accept-encoding: identity`, but a
// gateway can send a compressed body ANYWAY (z.ai's GLM Coding Plan gzips its
// /models JSON regardless — issue #30). Without an inflater those bytes reach
// the JSON parser as garbage: the model list comes back empty and the picker
// shows nothing, with no error. This decodes them.
//
// Self-contained: a canonical RFC 1951 DEFLATE decoder (ported from the same
// bounds-safe implementation rag-cpp uses for OOXML) plus the RFC 1952 (gzip)
// and RFC 1950 (zlib) wrapper handling. No zlib dependency — the airgap path
// deliberately avoids linking it, and a ~250-line self-contained decoder keeps
// the io layer free of an upward dependency on rag.

#include <optional>
#include <string>
#include <string_view>

namespace agentty::io::compress {

// Inflate a raw RFC 1951 DEFLATE stream. `max_out` caps the output so a
// malicious/looping stream can't exhaust memory (a decompression bomb).
// Returns std::nullopt on corrupt/oversized input.
[[nodiscard]] std::optional<std::string> inflate_deflate(
    std::string_view in, std::size_t max_out);

// Decode an HTTP response body per its `content-encoding` value:
//   "gzip" / "x-gzip"  → strip the RFC 1952 header+trailer, then DEFLATE
//   "deflate"          → RFC 1950 zlib wrapper if present, else raw DEFLATE
//   "identity" / ""    → returned unchanged
// Unknown encodings return std::nullopt (the caller keeps the raw body and
// logs). `max_out` bounds the result. Case-insensitive on the encoding name.
[[nodiscard]] std::optional<std::string> decode_content_encoding(
    std::string_view encoding, std::string_view body, std::size_t max_out);

}  // namespace agentty::io::compress
