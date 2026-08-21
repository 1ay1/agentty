// agentty::ui::host — cooperating-host integration escapes. See the header.
//
// Deliberately nlohmann-FREE: this is a tiny leaf TU on the reducer's path,
// and pulling <nlohmann/json.hpp> in just to serialize a 3-field object would
// cost far more compile time than it's worth. The payload is small and fully
// under our control, so we emit the JSON with a minimal, correct string
// escaper instead.
#include "agentty/runtime/view/host_escape.hpp"

#include <cstdio>
#include <cstdlib>

namespace agentty::ui::host {

namespace {

// Detect the cooperating host ONCE. AGENTTY_HOST is the explicit first-class
// signal (an editor launching agentty sets AGENTTY_HOST=emacs); INSIDE_EMACS
// is Emacs's own marker and only its "vterm" frontend can run our OSC hooks.
bool detect_integration() {
    if (const char* h = std::getenv("AGENTTY_HOST"); h && *h) {
        std::string_view v{h};
        if (v == "emacs" || v == "vterm") return true;
    }
    if (const char* ie = std::getenv("INSIDE_EMACS"); ie && *ie) {
        if (std::string_view{ie}.find("vterm") != std::string_view::npos)
            return true;
    }
    return false;
}

// Append `s` as a JSON string BODY (no surrounding quotes) to `out`, escaping
// the seven characters JSON requires plus all C0 controls as \uXXXX. Enough
// for a filesystem path + a tool name; no nlohmann needed.
void append_json_escaped(std::string& out, std::string_view s) {
    static const char* kHex = "0123456789abcdef";
    for (unsigned char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b";  break;
            case '\f': out += "\\f";  break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (c < 0x20) {
                    out += "\\u00";
                    out += kHex[(c >> 4) & 0xF];
                    out += kHex[c & 0xF];
                } else {
                    out += static_cast<char>(c);
                }
        }
    }
}

} // namespace

bool integration_active() {
    static const bool on = detect_integration();
    return on;
}

std::optional<std::string>
file_event_osc(std::string_view kind, std::string_view path,
               std::optional<int> line) {
    if (!integration_active() || path.empty()) return std::nullopt;

    // OSC 5379 ; agentty ; {"event":"file","kind":..,"path":..,"line":N}
    // We return only the OSC PAYLOAD (everything after "ESC ] <code> ;"); the
    // caller hands 5379 + this to maya's Cmd::emit_osc, which prepends the
    // "ESC ] 5379 ;" and appends the ST terminator.
    std::string out = "agentty;{\"event\":\"file\",\"kind\":\"";
    append_json_escaped(out, kind);
    out += "\",\"path\":\"";
    append_json_escaped(out, path);
    out += '"';
    if (line && *line > 0) {
        out += ",\"line\":";
        char buf[16];
        std::snprintf(buf, sizeof(buf), "%d", *line);
        out += buf;
    }
    out += '}';
    return out;
}

} // namespace agentty::ui::host
