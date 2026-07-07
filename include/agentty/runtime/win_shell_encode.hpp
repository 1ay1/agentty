#pragma once
// win_shell_encode — the pure, platform-independent PowerShell command
// encoder used by the Ctrl+G code-block runner's Windows path.
//
// A PowerShell block is run via `powershell -EncodedCommand <base64>`, where
// the base64 is of the UTF-16LE (little-endian) bytes of the script. This is
// PowerShell's documented contract: -EncodedCommand sidesteps every layer of
// cmd.exe / argv quoting (nested quotes, newlines, `$vars`, `&|<>` all pass
// through untouched) because the whole script is one opaque base64 argument.
//
// This lives in a header \u2014 not buried #if _WIN32 in codeblock.cpp \u2014 so the
// encoder can be unit-tested on ANY platform. A bug here (wrong endianness,
// bad base64 padding) would silently corrupt EVERY PowerShell block on
// Windows, and that branch can't be compiled on the Linux/macOS dev+CI hosts;
// making the transform pure + testable is the honest way to cover it.

#include <cstddef>
#include <string>

namespace agentty::win_shell {

// Base64-encode `bytes` (standard alphabet, '=' padded). Pure; no deps.
[[nodiscard]] inline std::string base64(const std::string& bytes) {
    static constexpr char kB64[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string enc;
    enc.reserve((bytes.size() + 2) / 3 * 4);
    for (std::size_t i = 0; i < bytes.size(); i += 3) {
        const unsigned b0 = static_cast<unsigned char>(bytes[i]);
        const unsigned b1 = i + 1 < bytes.size() ? static_cast<unsigned char>(bytes[i + 1]) : 0u;
        const unsigned b2 = i + 2 < bytes.size() ? static_cast<unsigned char>(bytes[i + 2]) : 0u;
        const unsigned triple = (b0 << 16) | (b1 << 8) | b2;
        enc.push_back(kB64[(triple >> 18) & 0x3F]);
        enc.push_back(kB64[(triple >> 12) & 0x3F]);
        enc.push_back(i + 1 < bytes.size() ? kB64[(triple >> 6) & 0x3F] : '=');
        enc.push_back(i + 2 < bytes.size() ? kB64[triple & 0x3F] : '=');
    }
    return enc;
}

// Widen `body` (treated as a stream of raw bytes, one char = one UTF-16 code
// unit \u2014 which is exactly right for ASCII scripts, and byte-faithful for the
// rest) to UTF-16LE bytes: low byte then high byte per code unit.
[[nodiscard]] inline std::string to_utf16le_bytes(const std::string& body) {
    std::string bytes;
    bytes.reserve(body.size() * 2);
    for (unsigned char c : body) {
        bytes.push_back(static_cast<char>(c));   // low byte
        bytes.push_back('\0');                    // high byte (LE)
    }
    return bytes;
}

// The full `-EncodedCommand` payload (base64 of UTF-16LE), no `powershell`
// prefix \u2014 so tests can assert the payload directly.
[[nodiscard]] inline std::string encoded_command(const std::string& body) {
    return base64(to_utf16le_bytes(body));
}

// The complete command line the Windows runner spawns for a PowerShell block.
[[nodiscard]] inline std::string powershell_command(const std::string& body) {
    return "powershell -NoProfile -ExecutionPolicy Bypass -EncodedCommand "
         + encoded_command(body);
}

} // namespace agentty::win_shell
