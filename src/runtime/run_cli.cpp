// agentty run: the CLI side. Reads the prompt, runs it, turns signals into a
// clean cancel, and prints the answer the way a script wants it.

#include "agentty/runtime/run_cli.hpp"

#include "agentty/tool/one_shot.hpp"

#include <maya/runtime.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <optional>
#include <stop_token>
#include <string>

namespace agentty::run_cli {

namespace pf = maya::platform;
using Status = tools::OneShotResult::Status;

namespace {

int exit_code_for(Status s, std::optional<maya::sig> got) {
    switch (s) {
        case Status::Ok:            return 0;
        case Status::Failed:        return 1;
        case Status::TurnLimit:     return 1;
        case Status::Unavailable:   return 3;
        case Status::ProviderError: return 3;
        case Status::Timeout:       return 124;   // same as timeout(1)
        case Status::Cancelled:
            if (got == maya::sig::terminate) return 143;
            if (got == maya::sig::hangup)    return 129;
            return 130;
    }
    return 1;
}

nlohmann::json to_json(const tools::OneShotResult& r, int code) {
    nlohmann::json j = {
        {"status",      tools::status_name(r.status)},
        {"exit_code",   code},
        {"result",      r.text},
        {"agent",       r.agent},
        {"model",       r.model},
        {"turns",       r.turns},
        {"tool_calls",  r.tool_calls},
        {"tool_errors", r.tool_errors},
        {"usage",       {{"input_tokens", r.input_tokens},
                         {"output_tokens", r.output_tokens}}},
        {"duration_ms", r.duration_ms},
    };
    if (!r.ok()) j["error"] = r.error;
    return j;
}

bool write_file(const std::string& path, const std::string& body) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f << body;
    if (!body.empty() && body.back() != '\n') f << '\n';
    return static_cast<bool>(f.flush());
}

// Runs the agent on this thread while a helper waits for Ctrl-C, SIGTERM or
// SIGHUP, and for --timeout. The loop only checks its clock between turns, so
// the helper cancels a long tool or a stuck stream when time is up. A second
// Ctrl-C while the run unwinds exits at once.
tools::OneShotResult run_with_signals(const Options& o, std::optional<maya::sig>& got) {
    using clock = std::chrono::steady_clock;
    const auto deadline = o.timeout.count() > 0
        ? std::optional<clock::time_point>(clock::now() + o.timeout) : std::nullopt;
    auto sigs = pf::native_signals::install(
        {maya::sig::interrupt, maya::sig::terminate, maya::sig::hangup});
    auto reactor = pf::native_reactor::create();
    if (!sigs || !reactor) {
        tools::OneShotOptions opts{o.prompt, o.agent, o.max_turns, o.timeout, {}};
        return tools::run_one_shot(opts);
    }
    constexpr std::uint64_t kSigToken = 1;
    auto reg = reactor->watch(sigs->handle(), pf::interest::read, kSigToken);
    const auto waker = reactor->waker();

    std::stop_source cancel;
    std::stop_source done;
    bool timed_out = false;   // written by the helper, read after join
    tools::OneShotResult result;
    maya::scope([&](maya::nursery& n) {
        auto watcher = n.spawn([&] {
            int seen = 0;
            while (!done.stop_requested()) {
                std::optional<std::chrono::milliseconds> wait;
                if (deadline && !cancel.stop_requested()) {
                    const auto left = *deadline - clock::now();
                    wait = std::max(std::chrono::milliseconds{0},
                                    std::chrono::ceil<std::chrono::milliseconds>(left));
                }
                auto w = reactor->wait(wait);
                if (!w || done.stop_requested()) break;
                if (deadline && !cancel.stop_requested() && clock::now() >= *deadline) {
                    timed_out = true;
                    cancel.request_stop();
                }
                for (auto s : sigs->take()) {
                    if (++seen == 1) {
                        got = s;
                        std::fputs("\nagentty run: stopping (again to force)\n", stderr);
                        cancel.request_stop();
                    } else {
                        std::fputs("agentty run: forced exit\n", stderr);
                        std::fflush(stderr);
                        std::_Exit(exit_code_for(Status::Cancelled, got));
                    }
                }
            }
        });
        tools::OneShotOptions opts{o.prompt, o.agent, o.max_turns, o.timeout,
                                   cancel.get_token()};
        try {
            result = tools::run_one_shot(opts);
        } catch (...) {
            done.request_stop();
            waker.wake();
            throw;
        }
        done.request_stop();
        waker.wake();
        watcher.join();
    });
    (void)reg;
    // Our own deadline cancel is a timeout, unless a signal came first.
    if (timed_out && !got && result.status == Status::Cancelled) {
        result.status = Status::Timeout;
        result.error  = "hit the time limit before finishing";
    }
    return result;
}

}  // namespace

std::optional<int> parse_count(std::string_view s) {
    int n = 0;
    auto [p, ec] = std::from_chars(s.data(), s.data() + s.size(), n);
    if (ec != std::errc{} || p != s.data() + s.size() || n < 1 || n > 500) return std::nullopt;
    return n;
}

std::optional<std::chrono::seconds> parse_duration(std::string_view s) {
    long long mult = 1;
    if (!s.empty()) {
        switch (s.back()) {
            case 's': mult = 1;    s.remove_suffix(1); break;
            case 'm': mult = 60;   s.remove_suffix(1); break;
            case 'h': mult = 3600; s.remove_suffix(1); break;
            default: break;
        }
    }
    long long n = 0;
    auto [p, ec] = std::from_chars(s.data(), s.data() + s.size(), n);
    if (s.empty() || ec != std::errc{} || p != s.data() + s.size() || n < 1
        || n > 7 * 24 * 3600 / mult)
        return std::nullopt;
    return std::chrono::seconds{n * mult};
}

int run(const Options& o) {
    std::optional<maya::sig> got;
    const tools::OneShotResult r = run_with_signals(o, got);
    const int code = exit_code_for(r.status, got);

    std::string body;
    if (o.json) {
        body = to_json(r, code).dump(2);
    } else {
        body = r.text;
    }

    if (!o.output.empty()) {
        if (!write_file(o.output, body)) {
            std::fprintf(stderr, "agentty run: can't write %s\n", o.output.c_str());
            return code == 0 ? 1 : code;
        }
    } else if (o.json || r.ok() || !r.text.empty()) {
        // Partial text still goes to stdout on failure; the exit code says
        // it isn't a finished answer.
        std::fputs(body.c_str(), stdout);
        if (!body.empty() && body.back() != '\n') std::fputc('\n', stdout);
        std::fflush(stdout);
    }

    if (!r.ok())
        std::fprintf(stderr, "agentty run: %s: %s\n",
                     std::string(tools::status_name(r.status)).c_str(), r.error.c_str());
    if (o.verbose) {
        std::fprintf(stderr,
                     "agentty run: %s, %s, %d turns, %d tool calls (%d failed), "
                     "%lld in / %lld out tokens, %.1fs\n",
                     r.agent.c_str(), r.model.empty() ? "?" : r.model.c_str(), r.turns,
                     r.tool_calls, r.tool_errors, r.input_tokens, r.output_tokens,
                     static_cast<double>(r.duration_ms) / 1000.0);
    }
    return code;
}

}  // namespace agentty::run_cli
