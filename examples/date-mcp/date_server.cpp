// date_server.cpp — a tiny MCP plugin for agentty (built with mcp-cpp).
//
// Gives the model something it can't know on its own: the current date &
// time. Two tools:
//   • current_date   — today's date/time (UTC + local), for anything
//                       date-sensitive.
//   • days_between    — whole days from one YYYY-MM-DD date to another
//                       (signed; b - a).
//
// Build + install: see README.md in this directory. Run agentty and ask
// "what day is it?" — the model calls mcp__date__current_date.

#include <mcp/mcp.hpp>

#include <chrono>
#include <cstdio>
#include <ctime>
#include <iostream>
#include <optional>
#include <string>

using namespace mcp;

namespace {

// Format a std::tm with strftime into a std::string.
std::string fmt(const std::tm& tm, const char* pat) {
    char buf[128];
    std::size_t n = std::strftime(buf, sizeof buf, pat, &tm);
    return std::string(buf, n);
}

// Parse "YYYY-MM-DD" into days since epoch (UTC midnight). nullopt on a
// malformed string.
std::optional<long long> parse_ymd_to_days(const std::string& s) {
    int y = 0, mo = 0, d = 0;
    if (std::sscanf(s.c_str(), "%d-%d-%d", &y, &mo, &d) != 3)
        return std::nullopt;
    if (mo < 1 || mo > 12 || d < 1 || d > 31) return std::nullopt;
    using namespace std::chrono;
    // C++20 calendar → sys_days is the day count since 1970-01-01.
    year_month_day ymd{year{y}, month{static_cast<unsigned>(mo)},
                       day{static_cast<unsigned>(d)}};
    if (!ymd.ok()) return std::nullopt;
    return sys_days{ymd}.time_since_epoch().count();
}

} // namespace

int main() {
    StdioTransport transport(std::cin, std::cout);
    Server server(transport.sink(),
                  Implementation{"date", "1.0.0", std::string("Date & Time"),
                                 Nothing, Nothing, Nothing});
    server.set_capabilities(ServerCapabilities{
        .tools = ToolsCapability{false},
    });
    server.set_instructions(
        "Date/time helpers — the model has no reliable clock of its own.");

    // ── tool: current_date ────────────────────────────────────────────
    {
        Tool t;
        t.name  = "current_date";
        t.title = "Current date";
        t.description =
            "Today's date and time (UTC + local + weekday). Call this "
            "for anything date-sensitive — the model cannot know 'now'.";
        t.inputSchema.properties = Json::object();
        t.annotations = ToolAnnotations{};
        t.annotations->readOnlyHint = true;
        server.register_tool(std::move(t), [](const Json&) -> CallToolResult {
            const std::time_t now = std::time(nullptr);
            std::tm utc{}, loc{};
#if defined(_WIN32)
            gmtime_s(&utc, &now);
            localtime_s(&loc, &now);
#else
            gmtime_r(&now, &utc);
            localtime_r(&now, &loc);
#endif
            CallToolResult r;
            r.content = {text(
                "UTC:   " + fmt(utc, "%Y-%m-%d %H:%M:%S") + "\n" +
                "Local: " + fmt(loc, "%Y-%m-%d %H:%M:%S %Z") +
                " (" + fmt(loc, "%A") + ")")};
            return r;
        });
    }

    // ── tool: days_between ────────────────────────────────────────────
    {
        Tool t;
        t.name  = "days_between";
        t.title = "Days between";
        t.description =
            "Whole days from date `a` to date `b` (signed, b minus a). "
            "Both dates are YYYY-MM-DD.";
        t.inputSchema.properties = Json{
            {"a", {{"type", "string"}, {"description", "start date, YYYY-MM-DD"}}},
            {"b", {{"type", "string"}, {"description", "end date, YYYY-MM-DD"}}},
        };
        t.inputSchema.required = List<std::string>{"a", "b"};
        t.annotations = ToolAnnotations{};
        t.annotations->readOnlyHint = true;
        server.register_tool(std::move(t), [](const Json& args) -> CallToolResult {
            std::string a, b;
            for (auto& [k, v] : args.items()) {
                if (k == "a" && v.is_string()) a = v.get<std::string>();
                if (k == "b" && v.is_string()) b = v.get<std::string>();
            }
            auto da = parse_ymd_to_days(a);
            auto db = parse_ymd_to_days(b);
            CallToolResult r;
            if (!da || !db) {
                r.content = {text("error: dates must be YYYY-MM-DD")};
                r.isError = true;
                return r;
            }
            const long long diff = *db - *da;
            r.content = {text(std::to_string(diff) + " day" +
                              (diff == 1 || diff == -1 ? "" : "s") +
                              " (" + b + " minus " + a + ")")};
            return r;
        });
    }

    transport.start(server.engine());
    transport.join();   // serve until agentty closes stdin
    return 0;
}
