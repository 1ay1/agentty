// fuzz_richpaste.cpp — libFuzzer harness for richpaste's parsers. Every one
// of them reads bytes someone else controls: the clipboard (BMP/DIB images,
// file URI lists), the ssh agent socket (wire protocol), the terminal
// (OSC 5522 replies, also with their ESC bytes stripped), and pasted text
// (armor). Build and run: tests/fuzz/run.sh
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

#include "rp/armor.hpp"
#include "rp/clipboard.hpp"
#include "rp/core.hpp"
#include "rp/image.hpp"
#include "rp/inband.hpp"
#include "rp/wire.hpp"

#define CHECK(c) do { if (!(c)) { __builtin_trap(); } } while (0)

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    if (size == 0) return 0;
    const std::uint8_t which = data[0];
    const std::string_view text{reinterpret_cast<const char*>(data) + 1, size - 1};
    const rp::ByteView bytes{reinterpret_cast<const std::byte*>(data) + 1, size - 1};

    switch (which % 8) {
    case 0: {   // images off the clipboard, and the PNG we turn them into
        for (auto img : {rp::image::decode_bmp(bytes), rp::image::decode_dib(bytes)})
            if (img) {
                CHECK(img->pixels.size() == std::size_t{img->width} * img->height * 4);
                if (std::size_t{img->width} * img->height <= 1u << 16)
                    CHECK(!rp::image::encode_png(*img).empty());
            }
        break;
    }
    case 1: {   // the agent socket protocol
        (void)rp::wire::decode_request(bytes);
        if (auto items = rp::wire::decode_response(bytes)) {
            // A decoded response must re-encode and decode to the same count.
            const auto again = rp::wire::decode_response(rp::wire::encode_items(*items));
            CHECK(again && again->size() == items->size());
        }
        break;
    }
    case 2: {   // OSC 5522 packets from the terminal
        if (auto p = rp::inband::parse_5522(text)) {
            rp::inband::ReadReply r;
            (void)r.add(*p, 1u << 20);
            (void)r.items();
            (void)r.mimes();
        }
        break;
    }
    case 3: {   // a 5522 reply that lost its ESC bytes, typed in as text
        (void)rp::inband::stripped_reply(text);
        (void)rp::inband::decode_stripped_reply(text, 1u << 20);
        break;
    }
    case 4: {   // armored files and images in pasted text
        (void)rp::armor::contains(text);
        (void)rp::armor::progress(text);
        auto s = rp::armor::split(text);
        for (const auto& part : s.parts)
            if (const auto* it = std::get_if<rp::Item>(&part)) {
                // Anything split() accepts must survive its own round trip.
                const auto back = rp::armor::split(rp::armor::encode(*it));
                CHECK(back.errors.empty() && back.parts.size() == 1);
            }
        break;
    }
    case 5: {   // core text types
        CHECK(rp::valid_utf8(rp::repair_utf8(text)));
        CHECK(rp::valid_utf8(rp::latin1_to_utf8(text)));
        (void)rp::Mime::parse(text);
        (void)rp::FileName::parse(text);
        // Decoding is lenient about the unused low bits of a padded group
        // ("Aue=" and "Auc=" both give the same bytes), so only check that
        // a decode re-encodes to something that decodes the same.
        if (auto d = rp::base64_decode(text)) {
            const auto again = rp::base64_decode(rp::base64_encode(*d));
            CHECK(again && *again == *d);
        }
        break;
    }
    case 6:     // file:// URI lists
        (void)rp::parse_uri_list(text);
        break;
    case 7: {   // mime picking off a listing
        std::vector<std::string> offered;
        std::size_t at = 0;
        while (at <= text.size()) {
            const auto nl = text.find('\n', at);
            offered.emplace_back(text.substr(at, nl == std::string_view::npos ? std::string_view::npos : nl - at));
            if (nl == std::string_view::npos) break;
            at = nl + 1;
        }
        (void)rp::inband::choose_mimes(offered);
        break;
    }
    }
    return 0;
}
