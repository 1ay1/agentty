// inflate_test — gzip / zlib / raw-DEFLATE decoding for HTTP response bodies.
//
// Regression coverage for issue #30: a custom OpenAI host (z.ai's GLM Coding
// Plan) gzips its /models JSON even when agentty sends `accept-encoding:
// identity`. Without an inflater the compressed bytes reach the JSON parser
// as garbage and the model list silently comes back empty. These vectors were
// produced by Python's gzip/zlib for the exact response shape the picker reads.

#include "agentty/io/inflate.hpp"

#include <string>
#include <vector>

#include <doctest/doctest.h>

using agentty::io::compress::decode_content_encoding;
using agentty::io::compress::inflate_deflate;

namespace {
// The uncompressed payload every vector below decodes back to.
constexpr char kJson[] =
    R"({"object":"list","data":[{"id":"glm-4.5"},{"id":"glm-4.6"},{"id":"glm-4.5-air"}]})";

std::string bytes(std::initializer_list<int> v) {
    std::string s;
    s.reserve(v.size());
    for (int b : v) s.push_back(static_cast<char>(static_cast<unsigned char>(b)));
    return s;
}

// gzip.compress(kJson) — RFC 1952 (10-byte header + DEFLATE + 8-byte trailer).
const std::string kGzip = bytes({
    31,139,8,0,0,0,0,0,2,255,171,86,202,79,202,74,77,46,81,178,82,202,201,44,46,
    81,210,81,74,73,44,73,84,178,138,174,86,202,76,1,10,166,231,228,234,154,232,
    153,42,213,234,160,8,152,161,11,152,234,38,102,22,41,213,198,214,2,0,105,104,
    14,198,81,0,0,0});

// zlib.compress(kJson) — RFC 1950 (2-byte header + DEFLATE + 4-byte Adler32).
const std::string kZlib = bytes({
    120,156,171,86,202,79,202,74,77,46,81,178,82,202,201,44,46,81,210,81,74,73,44,
    73,84,178,138,174,86,202,76,1,10,166,231,228,234,154,232,153,42,213,234,160,8,
    152,161,11,152,234,38,102,22,41,213,198,214,2,0,244,234,24,77});

// Raw DEFLATE (the zlib body with the 2-byte header and 4-byte trailer stripped).
const std::string kRaw = bytes({
    171,86,202,79,202,74,77,46,81,178,82,202,201,44,46,81,210,81,74,73,44,73,84,
    178,138,174,86,202,76,1,10,166,231,228,234,154,232,153,42,213,234,160,8,152,
    161,11,152,234,38,102,22,41,213,198,214,2,0});

constexpr std::size_t kCap = 64ull * 1024 * 1024;
}  // namespace

TEST_CASE("inflate: gzip response body round-trips") {
    auto out = decode_content_encoding("gzip", kGzip, kCap);
    REQUIRE(out.has_value());
    CHECK(*out == kJson);
    // Case-insensitive, whitespace-tolerant, and the x-gzip alias.
    CHECK(decode_content_encoding("  GZIP ", kGzip, kCap).value() == kJson);
    CHECK(decode_content_encoding("x-gzip", kGzip, kCap).value() == kJson);
}

TEST_CASE("inflate: deflate (zlib-wrapped) response body round-trips") {
    auto out = decode_content_encoding("deflate", kZlib, kCap);
    REQUIRE(out.has_value());
    CHECK(*out == kJson);
}

TEST_CASE("inflate: deflate falls back to RAW when there is no zlib wrapper") {
    // Some servers label raw DEFLATE as "deflate". The decoder detects the
    // absent zlib header and inflates the whole body.
    auto out = decode_content_encoding("deflate", kRaw, kCap);
    REQUIRE(out.has_value());
    CHECK(*out == kJson);
}

TEST_CASE("inflate: identity and empty encodings pass the body through") {
    CHECK(decode_content_encoding("identity", kJson, kCap).value() == kJson);
    CHECK(decode_content_encoding("", kJson, kCap).value() == kJson);
}

TEST_CASE("inflate: unknown encoding is a soft failure (nullopt)") {
    // brotli et al. aren't supported; the caller keeps the raw body + logs.
    CHECK(!decode_content_encoding("br", kGzip, kCap).has_value());
    CHECK(!decode_content_encoding("compress", kGzip, kCap).has_value());
}

TEST_CASE("inflate: corrupt / truncated streams fail cleanly, never crash") {
    // Truncated gzip (header only).
    CHECK(!decode_content_encoding("gzip", kGzip.substr(0, 12), kCap).has_value());
    // Garbage claiming to be gzip.
    CHECK(!decode_content_encoding("gzip", std::string("not gzip at all"), kCap)
               .has_value());
    // Bad magic.
    CHECK(!decode_content_encoding("gzip", std::string(20, '\x00'), kCap)
               .has_value());
    // Raw DEFLATE truncated mid-stream.
    CHECK(!inflate_deflate(kRaw.substr(0, 5), kCap).has_value());
}

TEST_CASE("inflate: the decompression-bomb cap is enforced") {
    // A tiny output cap makes even a legitimate stream refuse to over-allocate.
    CHECK(!decode_content_encoding("gzip", kGzip, /*max_out=*/8).has_value());
}
