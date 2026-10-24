// fuzz_text.cpp — libFuzzer harness for agentty's pure text parsers: the
// ones that take untrusted bytes (tool output, streamed tool-call JSON,
// pasted text, model ids from a provider). Checks invariants, not just
// crashes. Build and run: tests/fuzz/run.sh
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

#include "agentty/domain/model_name.hpp"
#include "agentty/tool/util/partial_json.hpp"
#include "agentty/tool/util/utf8.hpp"
#include "agentty/util/base64.hpp"

namespace u = agentty::tools::util;

#define CHECK(c) do { if (!(c)) { __builtin_trap(); } } while (0)

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    if (size == 0) return 0;
    // First byte picks a split point so two-argument functions get two inputs.
    const std::size_t split = size > 1 ? data[0] % size : 0;
    const std::string_view all{reinterpret_cast<const char*>(data) + 1, size - 1};
    const std::string_view a = all.substr(0, std::min(split, all.size()));
    const std::string_view b = all.substr(std::min(split, all.size()));

    // utf8: every output is valid UTF-8, cuts stay in bounds.
    CHECK(u::is_valid_utf8(u::sanitize_utf8(all)));
    CHECK(u::is_valid_utf8(u::to_valid_utf8(std::string{all})));
    const std::size_t lim = static_cast<std::size_t>(data[0]) * 7;
    const std::size_t cut = u::safe_utf8_cut(all, lim);
    CHECK(cut <= all.size());
    CHECK(all.size() <= lim ? cut == all.size() : cut <= lim);
    if (u::is_valid_utf8(all)) CHECK(u::is_valid_utf8(all.substr(0, cut)));
    const auto stripped = u::strip_terminal_controls(all);
    for (char c : stripped) {
        const auto uc = static_cast<unsigned char>(c);
        CHECK(uc != 0x1b && uc != 0x7f && (uc >= 0x20 || c == '\n' || c == '\t'));
    }
    const auto tail = u::progress_tail(all, lim);
    CHECK(tail.size() <= all.size());
    CHECK(tail.data() >= all.data() && tail.data() + tail.size() == all.data() + all.size());

    // partial JSON as it streams in, byte by byte through a growing prefix.
    (void)u::close_partial_json(all);
    (void)u::ended_inside_string(all);
    const std::string key{a.substr(0, std::min<std::size_t>(a.size(), 12))};
    (void)u::sniff_string(b, key);
    (void)u::sniff_string_progressive(b, key);
    if (auto at = u::locate_string_value(b, key)) {
        CHECK(*at <= b.size());
        const std::string whole = u::decode_string_from(b, *at);
        std::string inc;
        std::size_t through = *at;
        bool closed = false;
        for (std::size_t end = *at; end <= b.size(); end += 1 + end % 5) {
            (void)u::decode_string_append(b.substr(0, end), &through, inc, &closed);
            CHECK(through <= end);
        }
        (void)u::decode_string_append(b, &through, inc, &closed);
        CHECK(inc == whole);   // incremental decode matches the one-shot one
    }

    // base64 round trip.
    const std::string enc = agentty::util::base64_encode(all);
    CHECK(agentty::util::base64_decode(enc) == all);
    (void)agentty::util::base64_decode(all);

    // model id decoding (ids and display names come from provider APIs).
    // A tag-only id (":latest") decodes to an empty name on purpose, so
    // check what must always hold: bounded output, no control bytes.
    const auto mn = agentty::model_name::decode(a, b);
    CHECK(mn.name.size() <= 64 && mn.qualifier.size() <= 64 && mn.version.size() <= 64);
    for (char c : mn.full()) {
        const auto uc = static_cast<unsigned char>(c);
        CHECK(uc >= 0x20 && uc != 0x7f);
    }
    return 0;
}
