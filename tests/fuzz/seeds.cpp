// Writes seed inputs for the fuzz harnesses: real, well-formed examples of
// each format, so the fuzzer mutates from deep inside the parsers instead of
// rediscovering the syntax. Run by tests/fuzz/run.sh.
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

#include "rp/armor.hpp"
#include "rp/core.hpp"
#include "rp/wire.hpp"

namespace fs = std::filesystem;

static void put(const fs::path& dir, const std::string& name, char sel, std::string_view body) {
    std::ofstream f(dir / name, std::ios::binary);
    f.put(sel);
    f.write(body.data(), static_cast<std::streamsize>(body.size()));
}

int main(int argc, char** argv) {
    const fs::path rp_dir = argc > 1 ? argv[1] : "build-fuzz/corpus/fuzz_richpaste";
    const fs::path tx_dir = argc > 2 ? argv[2] : "build-fuzz/corpus/fuzz_text";
    fs::create_directories(rp_dir);
    fs::create_directories(tx_dir);

    // A text item, an image and a file, as armor and as wire bytes.
    rp::Item text_item = rp::Text::repair("hello, clipboard");
    rp::Item png_item = rp::Image{rp::Mime::parse("image/png").value(),
                                  rp::to_bytes(std::string_view{"\x89PNG\r\n\x1a\n0000IHDR"})};
    rp::Item file_item = rp::File{rp::FileName::parse("notes.txt").value(),
                                  rp::Mime::parse("text/plain").value(),
                                  rp::to_bytes(std::string_view{"a small file\n"})};
    const std::string armored = "before " + rp::armor::encode(png_item) + " after";
    put(rp_dir, "armor", 4, armored);
    put(rp_dir, "armor_two", 4, rp::armor::encode(text_item) + "\n" + rp::armor::encode(file_item));
    const rp::Item both[] = {text_item, png_item, file_item};
    const auto wire = rp::wire::encode_items(both);
    put(rp_dir, "wire", 1, std::string_view{reinterpret_cast<const char*>(wire.data()), wire.size()});
    const auto err = rp::wire::encode_error("clipboard is empty", rp::Code::empty);
    put(rp_dir, "wire_err", 1, std::string_view{reinterpret_cast<const char*>(err.data()), err.size()});

    // OSC 5522 packets, with and without ESC.
    const std::string data = rp::base64_encode(rp::to_bytes(std::string_view{"abc"}));
    put(rp_dir, "osc_data", 2, "type=read:status=DATA:mime=" + rp::base64_encode(rp::to_bytes(std::string_view{"image/png"})) + ";" + data);
    put(rp_dir, "osc_ok", 2, "type=read:status=OK");
    put(rp_dir, "stripped", 3, "]5522;type=read:status=OK\\]5522;type=read:status=DATA:mime=aW1hZ2UvcG5n;"
                                + data + "\\]5522;type=read:status=DONE\\");

    // A 2x1 24-bit BMP.
    const unsigned char bmp[] = {
        'B','M', 62,0,0,0, 0,0,0,0, 54,0,0,0,
        40,0,0,0, 2,0,0,0, 1,0,0,0, 1,0, 24,0, 0,0,0,0, 8,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0,
        255,0,0, 0,255,0, 0,0};
    put(rp_dir, "bmp", 0, std::string_view{reinterpret_cast<const char*>(bmp), sizeof bmp});
    put(rp_dir, "uris", 6, "file:///tmp/a%20b.png\r\nfile:///home/x/y.txt\r\n");
    put(rp_dir, "mimes", 7, "image/png\ntext/plain;charset=utf-8\ntext/uri-list");

    // fuzz_text: tool-call JSON as it streams, model ids, terminal output.
    put(tx_dir, "json", 1, "{\"path\":\"a.txt\",\"content\":\"line\\none \\\"q\\\" \\u00e9\"}");
    put(tx_dir, "json_cut", 2, "{\"path\":\"a.txt\",\"content\":\"half a li");
    put(tx_dir, "model", 18, "claude-sonnet-4-5-20250929Claude Sonnet 4.5");
    put(tx_dir, "model_q", 23, "qwen2.5-coder:32b-instruct-q4_K_M");
    put(tx_dir, "term", 0, "\x1b[1;31merror\x1b[0m: \x1b]0;title\x07ok\r\nprogress 10%\rprogress 99%\n\tdone\x7f");
    put(tx_dir, "utf8", 0, "caf\xc3\xa9 \xe6\xa8\xa1\xe5\x9e\x8b \xf0\x9f\x98\x80 \xc3 \xe6\xa8");
    return 0;
}
