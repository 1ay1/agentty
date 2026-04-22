#include "moha/io/anthropic.hpp"

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <sstream>
#include <vector>

#include <curl/curl.h>
#include <nlohmann/json.hpp>
#include <simdjson.h>

#include "moha/tool/registry.hpp"

namespace moha::anthropic {

namespace {

// Belt-and-suspenders UTF-8 scrubber. Registry already converts subprocess
// output at the capture boundary (GetConsoleOutputCP / CP_ACP pivot), but
// any string that reaches json::dump() must be valid UTF-8 or the API call
// dies with `type_error.316`. Cheap to run on already-valid strings, and
// guards future call sites that assemble tool output from multiple pieces
// (e.g. error suffix + partial output) where a byte boundary could split a
// UTF-8 sequence. Replaces invalid byte runs with U+FFFD.
std::string scrub_utf8(std::string_view in) {
    std::string out;
    out.reserve(in.size());
    auto repl = [&]{ out.append("\xEF\xBF\xBD"); };
    size_t i = 0;
    while (i < in.size()) {
        unsigned char c = (unsigned char)in[i];
        if (c < 0x80) { out.push_back((char)c); ++i; continue; }
        int extra; unsigned char mask; uint32_t min_cp;
        if      ((c & 0xE0) == 0xC0) { extra = 1; mask = 0x1F; min_cp = 0x80; }
        else if ((c & 0xF0) == 0xE0) { extra = 2; mask = 0x0F; min_cp = 0x800; }
        else if ((c & 0xF8) == 0xF0) { extra = 3; mask = 0x07; min_cp = 0x10000; }
        else { repl(); ++i; continue; }
        if (i + (size_t)extra >= in.size()) { repl(); ++i; continue; }
        uint32_t cp = c & mask;
        bool ok = true;
        for (int k = 1; k <= extra; ++k) {
            unsigned char d = (unsigned char)in[i + (size_t)k];
            if ((d & 0xC0) != 0x80) { ok = false; break; }
            cp = (cp << 6) | (d & 0x3F);
        }
        if (!ok || cp < min_cp || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
            repl(); ++i; continue;
        }
        out.append(in.data() + i, (size_t)(extra + 1));
        i += (size_t)extra + 1;
    }
    return out;
}
} // namespace

namespace {
// Env-var-gated request/SSE dump. Set MOHA_DEBUG_API=1 to write to
// $MOHA_DEBUG_FILE (or ./moha-api.log). Appends, never truncates.
FILE* debug_log() {
    static std::mutex m;
    static FILE* fp = nullptr;
    static bool tried = false;
    std::lock_guard<std::mutex> lk(m);
    if (tried) return fp;
    tried = true;
    const char* on = std::getenv("MOHA_DEBUG_API");
    if (!on || !*on || *on == '0') return nullptr;
    const char* path = std::getenv("MOHA_DEBUG_FILE");
    std::string p = (path && *path) ? std::string{path} : std::string{"moha-api.log"};
    fp = std::fopen(p.c_str(), "ab");
    return fp;
}
void dbg(const char* fmt, ...) {
    FILE* fp = debug_log();
    if (!fp) return;
    va_list ap; va_start(ap, fmt);
    std::vfprintf(fp, fmt, ap);
    va_end(ap);
    std::fflush(fp);
}
} // namespace

using json = nlohmann::json;

// Anthropic API version / beta headers. Rotate here — every caller picks them
// up. Keep groups small so a caller can opt in with less scope if needed.
namespace headers {
    inline constexpr const char* version           = "anthropic-version: 2023-06-01";
    inline constexpr const char* beta_oauth_full   =
        "anthropic-beta: oauth-2025-04-20,prompt-caching-2024-07-31,"
        "context-management-2025-06-27,compact-2026-01-12";
    inline constexpr const char* beta_apikey_full  =
        "anthropic-beta: prompt-caching-2024-07-31,"
        "context-management-2025-06-27,compact-2026-01-12";
    inline constexpr const char* beta_oauth_only   = "anthropic-beta: oauth-2025-04-20";
    inline constexpr const char* content_type_json = "content-type: application/json";
    inline constexpr const char* accept_sse        = "accept: text/event-stream";
} // namespace headers

namespace {

// --- SSE parser -------------------------------------------------------------
struct SseState {
    // Pre-reserve so typical chunk sizes (CURLOPT_BUFFERSIZE default 16 KB)
    // don't force a cascade of reallocations during a fast stream.
    SseState() { buf.reserve(32 * 1024); data_accum.reserve(8 * 1024); }
    std::string buf;
    std::string event_name;
    std::string data_accum;
};

size_t curl_write_cb(char* ptr, size_t size, size_t nmemb, void* userdata);

struct StreamCtx {
    EventSink sink;
    void* cancel; // unused — kept for future cancellation hook
    SseState sse;
    // Tool-use tracking (current block index in-flight)
    std::string current_tool_id;
    std::string current_tool_name;
    bool in_tool_use = false;
    // Terminal-event tracking — exactly one of finished/errored must fire.
    bool terminated = false;
    // Stashed from message_delta so we can hand it to StreamFinished. Lets
    // the reducer tell "natural end" / "tool_use" apart from "max_tokens"
    // (which leaves the in-flight tool_use block truncated).
    std::string stop_reason;
    // simdjson parser is stateful and caches its scratch buffer across
    // iterate() calls — reusing one per stream avoids a malloc per SSE frame.
    // Co-located with StreamCtx because run_stream_sync is single-threaded.
    simdjson::ondemand::parser simd_parser;
    simdjson::padded_string     simd_scratch;
};

// Fast path: content_block_delta dominates stream volume (one per output
// token).  simdjson's ondemand walks the bytes in-place, grabs the two
// strings we need, and returns without ever materialising a DOM.  Falls
// back to caller for anything unexpected (unknown delta.type).
// Returns true if the event was fully handled.
bool dispatch_content_block_delta_fast(StreamCtx& ctx, const std::string& data) {
    // simdjson needs SIMDJSON_PADDING bytes of slack past end-of-buffer;
    // grow the scratch buffer to fit and copy there each frame. The scratch
    // lives on StreamCtx so the allocator only grows once per stream.
    const std::size_t need = data.size() + simdjson::SIMDJSON_PADDING;
    if (ctx.simd_scratch.size() < need) {
        ctx.simd_scratch = simdjson::padded_string(need);
    }
    std::memcpy(ctx.simd_scratch.data(), data.data(), data.size());
    std::memset(ctx.simd_scratch.data() + data.size(), 0, simdjson::SIMDJSON_PADDING);

    simdjson::ondemand::document doc;
    if (ctx.simd_parser.iterate(ctx.simd_scratch.data(), data.size(), need).get(doc))
        return false;

    simdjson::ondemand::object root;
    if (doc.get_object().get(root)) return false;

    // Pull `delta.type` first (ondemand is forward-only). Anthropic always
    // emits `delta` before the top-level `index`, but guard anyway.
    simdjson::ondemand::object delta;
    if (root["delta"].get_object().get(delta)) return false;

    std::string_view delta_type;
    if (delta["type"].get_string().get(delta_type)) return false;

    if (delta_type == "text_delta") {
        std::string_view text;
        if (delta["text"].get_string().get(text)) return false;
        ctx.sink(StreamTextDelta{std::string{text}});
        return true;
    }
    if (delta_type == "input_json_delta") {
        std::string_view partial;
        if (delta["partial_json"].get_string().get(partial)) return false;
        ctx.sink(StreamToolUseDelta{std::string{partial}});
        return true;
    }
    // Unknown delta type — let the nlohmann fallback log/ignore.
    return false;
}

void dispatch_event(StreamCtx& ctx, const std::string& name, const std::string& data) {
    if (data.empty() || data == "[DONE]") return;
    dbg("<< event=%s data=%s\n", name.c_str(), data.c_str());

    // Hot path first — ~95% of events during a streaming turn.
    if (name == "content_block_delta"
        && dispatch_content_block_delta_fast(ctx, data)) {
        return;
    }

    // Cold paths: parsed once per tool call or per message. nlohmann is
    // easier to audit and the cost is negligible at this cardinality.
    json j;
    try { j = json::parse(data); } catch (...) { return; }
    if (name == "message_start") {
        ctx.sink(StreamStarted{});
        if (j.contains("message") && j["message"].contains("usage")) {
            int in = j["message"]["usage"].value("input_tokens", 0);
            ctx.sink(StreamUsage{in, 0});
        }
    } else if (name == "content_block_start") {
        auto block = j.value("content_block", json::object());
        auto type = block.value("type", "");
        if (type == "tool_use") {
            ctx.current_tool_id = block.value("id", "");
            ctx.current_tool_name = block.value("name", "");
            ctx.in_tool_use = true;
            ctx.sink(StreamToolUseStart{ToolCallId{ctx.current_tool_id}, ToolName{ctx.current_tool_name}});
        }
    } else if (name == "content_block_delta") {
        // Fast path returned false — fallback in case Anthropic adds a new
        // delta type we don't recognise. Mirrors the old logic exactly.
        auto delta = j.value("delta", json::object());
        auto type = delta.value("type", "");
        if (type == "text_delta") {
            ctx.sink(StreamTextDelta{delta.value("text", "")});
        } else if (type == "input_json_delta") {
            ctx.sink(StreamToolUseDelta{delta.value("partial_json", "")});
        }
    } else if (name == "content_block_stop") {
        if (ctx.in_tool_use) {
            ctx.sink(StreamToolUseEnd{});
            ctx.in_tool_use = false;
            ctx.current_tool_id.clear();
            ctx.current_tool_name.clear();
        }
    } else if (name == "message_delta") {
        if (j.contains("usage")) {
            int out = j["usage"].value("output_tokens", 0);
            ctx.sink(StreamUsage{0, out});
        }
        // Capture `delta.stop_reason` for the upcoming message_stop. Anthropic
        // sends this here, not on message_stop itself.
        if (j.contains("delta") && j["delta"].contains("stop_reason")
            && j["delta"]["stop_reason"].is_string()) {
            ctx.stop_reason = j["delta"]["stop_reason"].get<std::string>();
        }
    } else if (name == "message_stop") {
        // If the upstream skipped content_block_stop for a tool_use block
        // (proxy cutoff, etc.), synthesize one so the reducer parses
        // args_streaming → args before finalize wipes the buffer.
        if (ctx.in_tool_use) {
            ctx.sink(StreamToolUseEnd{});
            ctx.in_tool_use = false;
            ctx.current_tool_id.clear();
            ctx.current_tool_name.clear();
        }
        ctx.sink(StreamFinished{ctx.stop_reason});
        ctx.terminated = true;
    } else if (name == "error") {
        auto err = j.value("error", json::object());
        ctx.sink(StreamError{err.value("message", "unknown error")});
        ctx.terminated = true;
    }
}

void feed_sse(StreamCtx& ctx, const char* data, size_t len) {
    ctx.sse.buf.append(data, len);
    size_t pos = 0;
    while (true) {
        size_t nl = ctx.sse.buf.find('\n', pos);
        if (nl == std::string::npos) break;
        std::string line = ctx.sse.buf.substr(pos, nl - pos);
        pos = nl + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) {
            if (!ctx.sse.data_accum.empty() || !ctx.sse.event_name.empty())
                dispatch_event(ctx, ctx.sse.event_name, ctx.sse.data_accum);
            ctx.sse.event_name.clear();
            ctx.sse.data_accum.clear();
        } else if (line.rfind("event:", 0) == 0) {
            size_t s = 6; while (s < line.size() && line[s] == ' ') ++s;
            ctx.sse.event_name = line.substr(s);
        } else if (line.rfind("data:", 0) == 0) {
            size_t s = 5; while (s < line.size() && line[s] == ' ') ++s;
            if (!ctx.sse.data_accum.empty()) ctx.sse.data_accum += "\n";
            ctx.sse.data_accum += line.substr(s);
        }
    }
    ctx.sse.buf.erase(0, pos);
}

size_t curl_write_cb(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* ctx = static_cast<StreamCtx*>(userdata);
    (void)ctx->cancel;
    feed_sse(*ctx, ptr, size * nmemb);
    return size * nmemb;
}

json tool_spec_to_json(const ToolSpec& s) {
    json j;
    j["name"] = s.name;  // std::string, serializes directly
    j["description"] = s.description;
    j["input_schema"] = s.input_schema;
    return j;
}

} // namespace

json build_messages(const Thread& t) {
    json msgs = json::array();
    for (const auto& m : t.messages) {
        json jm;
        jm["role"] = (m.role == Role::User) ? "user" : "assistant";
        json content = json::array();
        if (!m.text.empty()) {
            content.push_back({{"type", "text"}, {"text", scrub_utf8(m.text)}});
        }
        for (const auto& tc : m.tool_calls) {
            if (m.role == Role::Assistant) {
                // Anthropic requires tool_use.input to be an object — coerce
                // null/array/scalar (e.g. from a tool with no args, where no
                // input_json_delta arrived) into an empty object.
                json input = tc.args.is_object() ? tc.args : json::object();
                content.push_back({
                    {"type", "tool_use"},
                    {"id", tc.id.value},
                    {"name", tc.name.value},
                    {"input", std::move(input)},
                });
            }
        }
        // Append tool_result blocks as user messages (Anthropic convention).
        if (!content.empty()) { jm["content"] = content; msgs.push_back(jm); }

        if (m.role == Role::Assistant && !m.tool_calls.empty()) {
            json user_msg;
            user_msg["role"] = "user";
            json results = json::array();
            for (const auto& tc : m.tool_calls) {
                if (tc.status == ToolUse::Status::Done ||
                    tc.status == ToolUse::Status::Error ||
                    tc.status == ToolUse::Status::Rejected) {
                    results.push_back({
                        {"type", "tool_result"},
                        {"tool_use_id", tc.id.value},
                        {"content", tc.output.empty()
                            ? std::string{"(no output)"}
                            : scrub_utf8(tc.output)},
                        {"is_error", tc.status == ToolUse::Status::Error ||
                                     tc.status == ToolUse::Status::Rejected},
                    });
                }
            }
            if (!results.empty()) {
                user_msg["content"] = results;
                msgs.push_back(user_msg);
            }
        }
    }
    return msgs;
}

std::string default_system_prompt() {
    // Platform / shell hints so the model generates the right command syntax.
    // Without these, the bash tool gets POSIX-only commands (uname, sw_vers,
    // cat /etc/os-release) on Windows, which fail under cmd.exe.
#if defined(_WIN32)
    constexpr const char* os_name  = "Windows";
    constexpr const char* shell    = "cmd.exe (Windows Command Prompt)";
    constexpr const char* shell_hint =
        "Prefer native Windows equivalents: `dir` / `where` / `systeminfo` / "
        "`type` / `findstr` / `powershell -c`. Do NOT use POSIX-only tools "
        "like `uname`, `cat /etc/os-release`, `sw_vers`, `ls`, `grep`, `sed`, "
        "`awk`, or shell heredocs (`<<EOF`) — they will fail. "
        "Commands chain with `&&` and `||` under cmd.exe, but path separators "
        "are backslashes and paths with spaces must be quoted.";
#elif defined(__APPLE__)
    constexpr const char* os_name  = "macOS (Darwin)";
    constexpr const char* shell    = "sh";
    constexpr const char* shell_hint =
        "Use POSIX tools; `sw_vers` gives macOS version, `uname -a` gives kernel.";
#else
    constexpr const char* os_name  = "Linux";
    constexpr const char* shell    = "sh";
    constexpr const char* shell_hint =
        "Use POSIX tools; `/etc/os-release` gives distro info, `uname -a` gives kernel.";
#endif

    std::string cwd;
    try { cwd = std::filesystem::current_path().string(); } catch (...) {}

    std::ostringstream oss;
    oss << "You are Moha, a terminal coding assistant based on Claude, "
        << "working in the user's current directory like Zed's agent or "
        << "Claude Code. Be concise; let tool cards speak for themselves "
        << "rather than narrating every step.\n\n"
        << "<file-editing>\n"
        << "  - Modify existing files with `edit` (one or more "
        << "old_text→new_text substitutions). It produces a clean diff and "
        << "streams less data than rewriting the whole file.\n"
        << "  - Use `write` ONLY when (a) creating a brand-new file, or "
        << "(b) regenerating an entire file from scratch (format conversion, "
        << "full code generation). Never use `write` to change a few lines "
        << "of an existing file.\n"
        << "  - When editing, `read` the file first if you don't already "
        << "have its current contents — `edit.old_text` must match exactly.\n"
        << "  - NEVER shell out (cat/echo/sed/heredoc/printf) for file IO. "
        << "One `write` or `edit` call per file change.\n"
        << "  - ALWAYS include a brief `display_description` on `write` "
        << "and `edit` calls (e.g. 'Add retry on 429'). It shows in the "
        << "tool card while the file streams, so the user sees what you "
        << "are doing before the bytes finish arriving.\n"
        << "  - Tool inputs stream as JSON. The schemas list `path` first "
        << "for a reason: emit it (and `display_description`) before the "
        << "long fields (`content`, `edits`) so the UI paints meaningful "
        << "context immediately. Don't reorder.\n"
        << "</file-editing>\n\n"
        << "<shell>\n"
        << "  - Use `bash` for commands. Explain destructive ones before "
        << "running.\n"
        << "  - For listing/searching files, prefer the dedicated tools "
        << "(`list_dir`, `glob`, `grep`, `find_definition`) over shelling "
        << "out — they give the UI structured cards.\n"
        << "</shell>\n\n"
        << "<environment>\n"
        << "  os: " << os_name << "\n"
        << "  shell: " << shell << "\n";
    if (!cwd.empty()) oss << "  cwd: " << cwd << "\n";
    oss << "</environment>\n\n"
        << "<shell-notes>\n"
        << shell_hint << "\n"
        << "</shell-notes>\n";
    return oss.str();
}

std::vector<ToolSpec> default_tools() {
    std::vector<ToolSpec> out;
    for (const auto& td : tools::registry()) {
        out.push_back({td.name.value, td.description, td.input_schema});
    }
    return out;
}

// ----------------------------------------------------------------------------

void run_stream_sync(Request req, EventSink sink) {
    if (req.auth_header.empty()) {
        sink(StreamError{"not authenticated — run 'moha login' or set ANTHROPIC_API_KEY"});
        return;
    }
    CURL* curl = curl_easy_init();
    if (!curl) { sink(StreamError{"curl_easy_init failed"}); return; }

    // Emit StreamFinished as a fallback if the SSE stream never produced one
    // (e.g. proxy buffering, server cutoff). UI must not be left spinning.
    auto emit_terminal = [&](StreamCtx& ctx, std::optional<std::string> err) {
        if (ctx.terminated) return;
        if (err) sink(StreamError{*err});
        else     sink(StreamFinished{ctx.stop_reason});
        ctx.terminated = true;
    };

    const bool is_oauth = (req.auth_style == auth::Style::Bearer);

    json body;
    body["model"] = req.model;  // std::string from Request
    body["max_tokens"] = req.max_tokens;
    body["stream"] = true;

    // For OAuth, prepend a billing-header text block so subscription billing
    // is attributed correctly; the real system prompt follows.
    if (is_oauth) {
        json sys = json::array();
        sys.push_back({
            {"type", "text"},
            {"text", "x-anthropic-billing-header: cc_version=0.1.0;"
                     " cc_entrypoint=cli; cch=00000;"}
        });
        sys.push_back({
            {"type", "text"},
            {"text", req.system_prompt},
            {"cache_control", {{"type", "ephemeral"}}}
        });
        body["system"] = std::move(sys);
    } else {
        body["system"] = req.system_prompt;
    }

    body["messages"] = build_messages(Thread{ThreadId{""}, "", req.messages, {}, {}});
    if (!req.tools.empty()) {
        json tools_j = json::array();
        for (const auto& t : req.tools) tools_j.push_back(tool_spec_to_json(t));
        body["tools"] = std::move(tools_j);
    }
    std::string body_str = body.dump();

    dbg("==== request ====\n%s\n==== /request ====\n", body_str.c_str());

    // simdjson types have explicit default ctors, so designated-init skips
    // them — write each field long-hand.
    StreamCtx ctx{
        sink,                               // sink
        nullptr,                            // cancel
        SseState{},                         // sse
        std::string{},                      // current_tool_id
        std::string{},                      // current_tool_name
        false,                              // in_tool_use
        false,                              // terminated
        std::string{},                      // stop_reason
        simdjson::ondemand::parser{},       // simd_parser
        simdjson::padded_string{},          // simd_scratch
    };

    struct curl_slist* headers = nullptr;
    std::string auth_hdr = is_oauth
        ? (std::string("Authorization: ") + req.auth_header)
        : (std::string("x-api-key: ") + req.auth_header);
    headers = curl_slist_append(headers, auth_hdr.c_str());
    headers = curl_slist_append(headers, anthropic::headers::version);
    headers = curl_slist_append(headers,
        is_oauth ? anthropic::headers::beta_oauth_full
                 : anthropic::headers::beta_apikey_full);
    headers = curl_slist_append(headers, anthropic::headers::content_type_json);
    headers = curl_slist_append(headers, anthropic::headers::accept_sse);
    // Suppress libcurl's auto-added `Expect: 100-continue` for POST bodies
    // > 1 KB. Anthropic's edge does NOT honor 100-continue, so libcurl
    // would wait its 1-second internal timeout before giving up and sending
    // the body anyway — a flat 1s of latency on every turn. Sending the
    // empty header value tells libcurl to omit the line entirely.
    headers = curl_slist_append(headers, "Expect:");

    curl_easy_setopt(curl, CURLOPT_URL, "https://api.anthropic.com/v1/messages");
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body_str.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)body_str.size());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &ctx);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 1L);
    // NOSIGNAL: we run on background threads; libcurl's default DNS path
    // installs a SIGALRM handler that is not thread-safe. Force the
    // alarm-free code path.
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    // Bound the *connect* phase only. Once we're streaming, silence between
    // SSE events is normal (extended thinking, large tool_input streaming),
    // so we deliberately leave CURLOPT_TIMEOUT unset. 10 s is enough for any
    // healthy network and short enough that a dead-network user gets a real
    // error instead of an indefinite spinner.
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
#ifdef CURLOPT_TCP_FASTOPEN
    // TCP Fast Open: piggyback POST data on the SYN when the kernel has a
    // valid TFO cookie for api.anthropic.com (saves 1 RTT on cold connect,
    // no-op on warm reuse). Linux kernel needs net.ipv4.tcp_fastopen=1 to
    // honor the client side; harmless if disabled.
    curl_easy_setopt(curl, CURLOPT_TCP_FASTOPEN, 1L);
#endif
    // TCP keepalive is enough to detect genuinely dead sockets without
    // punishing legit silence between SSE events — Anthropic stalls output
    // during extended thinking, bulk tool-input streaming of large files,
    // and model compute spikes. A 90 s low-speed guard here killed the
    // connection mid-tool-input-stream and delivered a truncated args
    // buffer to finalize (manifesting as "content required" on write). Use
    // short keepalive probes so a *truly* dead connection is still caught
    // in < 2 minutes, without interrupting a slow but live stream.
    curl_easy_setopt(curl, CURLOPT_TCP_KEEPALIVE, 1L);
    curl_easy_setopt(curl, CURLOPT_TCP_KEEPIDLE,  30L);
    curl_easy_setopt(curl, CURLOPT_TCP_KEEPINTVL, 15L);
    curl_easy_setopt(curl, CURLOPT_TCP_NODELAY, 1L);
    // HTTP/2 over TLS (negotiated via ALPN, falls back to 1.1 if the peer
    // doesn't advertise h2). HTTP/2 is what Claude Code, Zed, and the
    // official Anthropic SDKs use — multiplexed streams survive idle
    // intermediary timeouts better than long-lived 1.1 connections, and
    // each DATA frame flushes to our callback as it arrives so SSE latency
    // is unchanged. The previous 1.1 + identity combo was needed only as a
    // workaround for gzip buffering, which doesn't apply over h2 framing.
    curl_easy_setopt(curl, CURLOPT_HTTP_VERSION, (long)CURL_HTTP_VERSION_2TLS);
    curl_easy_setopt(curl, CURLOPT_PIPEWAIT, 1L);  // wait for h2 negotiation
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "identity");
    // Read buffer for the SSE callback. Each HTTP/2 DATA frame already flushes
    // to the callback as it arrives, so this is the *upper bound* per-call,
    // not a coalescing buffer — Anthropic SSE frames are typically 200–800 B,
    // so even a single 16 KB buffer fits dozens at no latency cost. Bigger
    // than 4 KB reduces syscall/callback overhead under fast streams.
    curl_easy_setopt(curl, CURLOPT_BUFFERSIZE, 16L * 1024L);
    auth::apply_tls_options(curl);
    auth::apply_shared_cache(curl);

    CURLcode rc = curl_easy_perform(curl);
    long http = 0;
    long http_ver = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http);
    curl_easy_getinfo(curl, CURLINFO_HTTP_VERSION, &http_ver);
    dbg("==== curl rc=%d http=%ld proto=HTTP/%s ====\n",
        (int)rc, http,
        http_ver == CURL_HTTP_VERSION_2_0 ? "2"
        : http_ver == CURL_HTTP_VERSION_1_1 ? "1.1"
        : http_ver == CURL_HTTP_VERSION_3 ? "3" : "?");
    if (rc != CURLE_OK && rc != CURLE_WRITE_ERROR) {
        emit_terminal(ctx, std::string("http: ") + curl_easy_strerror(rc));
    } else if (http >= 400) {
        dbg("error body: %s\n", ctx.sse.buf.c_str());
        std::string body_err = ctx.sse.buf;
        std::string msg = std::string("HTTP ") + std::to_string(http);
        try {
            auto j = json::parse(body_err);
            if (j.contains("error") && j["error"].contains("message"))
                msg += ": " + j["error"]["message"].get<std::string>();
            else if (j.contains("message"))
                msg += ": " + j["message"].get<std::string>();
            else
                msg += ": " + body_err.substr(0, 300);
        } catch (...) {
            if (!body_err.empty()) msg += ": " + body_err.substr(0, 300);
        }
        if (http == 401 || http == 403)
            msg += "  (run 'moha login' to re-authenticate)";
        emit_terminal(ctx, std::move(msg));
    } else {
        // 2xx and the SSE parser may or may not have produced message_stop.
        // Guarantee one terminal event so the UI can finalize the turn.
        emit_terminal(ctx, std::nullopt);
    }
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
}

namespace {
size_t write_string_cb(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* buf = static_cast<std::string*>(userdata);
    buf->append(ptr, size * nmemb);
    return size * nmemb;
}
} // namespace

std::vector<ModelInfo> list_models(const std::string& auth_header,
                                   auth::Style auth_style) {
    std::vector<ModelInfo> result;
    if (auth_header.empty()) return result;

    CURL* curl = curl_easy_init();
    if (!curl) return result;

    const bool is_oauth = (auth_style == auth::Style::Bearer);
    struct curl_slist* headers = nullptr;
    std::string auth_hdr = is_oauth
        ? (std::string("Authorization: ") + auth_header)
        : (std::string("x-api-key: ") + auth_header);
    headers = curl_slist_append(headers, auth_hdr.c_str());
    headers = curl_slist_append(headers, anthropic::headers::version);
    if (is_oauth)
        headers = curl_slist_append(headers, anthropic::headers::beta_oauth_only);
    headers = curl_slist_append(headers, anthropic::headers::content_type_json);

    std::string response;
    curl_easy_setopt(curl, CURLOPT_URL, "https://api.anthropic.com/v1/models?limit=100");
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_string_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5L);
    curl_easy_setopt(curl, CURLOPT_HTTP_VERSION, (long)CURL_HTTP_VERSION_2TLS);
    curl_easy_setopt(curl, CURLOPT_PIPEWAIT, 1L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
#ifdef CURLOPT_TCP_FASTOPEN
    curl_easy_setopt(curl, CURLOPT_TCP_FASTOPEN, 1L);
#endif
    auth::apply_tls_options(curl);
    auth::apply_shared_cache(curl);

    CURLcode rc = curl_easy_perform(curl);
    long http = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (rc != CURLE_OK || http != 200) return result;

    try {
        auto j = json::parse(response);
        for (const auto& m : j.value("data", json::array())) {
            auto id = m.value("id", "");
            auto name = m.value("display_name", id);
            if (id.empty()) continue;
            result.push_back(ModelInfo{
                .id = ModelId{id},
                .display_name = name,
                .provider = "anthropic",
            });
        }
    } catch (...) {}

    return result;
}

} // namespace moha::anthropic
