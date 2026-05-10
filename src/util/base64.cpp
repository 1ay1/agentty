#include "agentty/util/base64.hpp"

#include <array>
#include <cctype>
#include <cstdint>

namespace agentty::util {

namespace {

constexpr char kAlphabet[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
    "abcdefghijklmnopqrstuvwxyz"
    "0123456789+/";

// Inverse table — built once. -1 = invalid, -2 = whitespace (skip),
// -3 = padding ('=').
constexpr auto kDecodeTable = []{
    std::array<int8_t, 256> t{};
    for (auto& v : t) v = -1;
    for (int i = 0; i < 64; ++i)
        t[static_cast<unsigned char>(kAlphabet[i])] = static_cast<int8_t>(i);
    t[static_cast<unsigned char>('=')] = -3;
    for (unsigned char ws : {' ', '\t', '\n', '\r', '\f', '\v'})
        t[ws] = -2;
    return t;
}();

} // namespace

std::string base64_encode(const unsigned char* data, std::size_t len) {
    std::string out;
    out.reserve(((len + 2) / 3) * 4);
    std::size_t i = 0;
    while (i + 3 <= len) {
        std::uint32_t v = (std::uint32_t{data[i]}     << 16)
                        | (std::uint32_t{data[i + 1]} <<  8)
                        |  std::uint32_t{data[i + 2]};
        out.push_back(kAlphabet[(v >> 18) & 0x3f]);
        out.push_back(kAlphabet[(v >> 12) & 0x3f]);
        out.push_back(kAlphabet[(v >>  6) & 0x3f]);
        out.push_back(kAlphabet[ v        & 0x3f]);
        i += 3;
    }
    if (i < len) {
        std::uint32_t v = std::uint32_t{data[i]} << 16;
        if (i + 1 < len) v |= std::uint32_t{data[i + 1]} << 8;
        out.push_back(kAlphabet[(v >> 18) & 0x3f]);
        out.push_back(kAlphabet[(v >> 12) & 0x3f]);
        if (i + 1 < len) out.push_back(kAlphabet[(v >> 6) & 0x3f]);
        else             out.push_back('=');
        out.push_back('=');
    }
    return out;
}

std::string base64_decode(std::string_view encoded) {
    std::string out;
    out.reserve((encoded.size() / 4) * 3);
    std::uint32_t buf = 0;
    int bits = 0;
    bool seen_pad = false;
    for (char c : encoded) {
        auto u = static_cast<unsigned char>(c);
        int v = kDecodeTable[u];
        if (v == -2) continue;            // whitespace
        if (v == -3) { seen_pad = true; continue; }
        if (v < 0 || seen_pad) return {}; // invalid char or data after pad
        buf = (buf << 6) | static_cast<std::uint32_t>(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<char>((buf >> bits) & 0xff));
        }
    }
    return out;
}

} // namespace agentty::util
