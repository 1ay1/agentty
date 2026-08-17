// hooks.cpp — user-authored lifecycle hooks. See hooks.hpp for the design
// and the consent-gate rationale. Implementation notes:
//
//   • The payload travels via AGENTTY_HOOK_PAYLOAD_FILE (a mode-0600 temp
//     file) instead of stdin: run_shell_command has no stdin-pipe seam,
//     and a file keeps multi-MB tool results out of the environment block
//     (env vars have hard platform caps).
//   • Hook commands run through run_shell_command — the SAME bwrap /
//     sandbox-exec wrapper the bash tool uses — so an approved hook is
//     workspace-confined exactly like model-driven shell.
//   • Approval store: content-hash trust via the SHARED scope::Approvals
//     primitive (the same one plugins use), persisted at
//     ~/.agentty/hooks_approved.json as a list of approved file hashes. Any
//     byte change to a hooks file yields a new hash → re-gated. A pre-scope
//     {path: hash} store is migrated transparently on first read.

#include "agentty/tool/hooks.hpp"

#include "agentty/auth/auth.hpp"            // auth::sha256_hex (file-content hash)
#include "agentty/scope/scope.hpp"          // scope::Approvals (shared trust store)
#include "agentty/tool/util/sandbox.hpp"    // run_shell_command
#include "agentty/tool/util/subprocess.hpp"

#include <nlohmann/json.hpp>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

#ifndef _WIN32
#include <sys/stat.h>   // chmod
#include <unistd.h>
#endif

namespace fs = std::filesystem;
using nlohmann::json;

namespace agentty::tools::hooks {

namespace {

constexpr std::size_t kMaxHooksFileBytes = 64 * 1024;
constexpr std::size_t kMaxPayloadBytes   = 4 * 1024 * 1024;
constexpr auto        kHookTimeout       = std::chrono::seconds{30};

struct HookEntry {
    std::string match;   // ERE on the tool name
    std::string run;     // sh -c command
};

struct HooksFile {
    std::string            path;      // absolute path of the file loaded
    std::string            hash;      // sha256 of the raw bytes
    std::vector<HookEntry> pre_tool;
    std::vector<HookEntry> post_tool;
    bool                   ok = false;
};

[[nodiscard]] fs::path home_dir() {
    if (auto* h = std::getenv("HOME"); h && *h) return fs::path{h};
    if (auto* h = std::getenv("USERPROFILE"); h && *h) return fs::path{h};
    return {};
}

[[nodiscard]] bool hooks_disabled() {
    const char* off = std::getenv("AGENTTY_NO_HOOKS");
    return off && off[0] && off[0] != '0';
}

[[nodiscard]] std::string read_all(const fs::path& p, std::size_t cap) {
    std::error_code ec;
    if (!fs::is_regular_file(p, ec) || ec) return {};
    auto sz = fs::file_size(p, ec);
    if (ec || sz == 0 || sz > cap) return {};
    std::ifstream f(p, std::ios::binary);
    if (!f) return {};
    std::string out(static_cast<std::size_t>(sz), '\0');
    f.read(out.data(), static_cast<std::streamsize>(sz));
    out.resize(static_cast<std::size_t>(f.gcount()));
    return out;
}

// Locate + parse the active hooks file. NOT cached across calls on
// purpose: hooks fire once per tool call (seconds apart), a stat+parse is
// noise there, and skipping the cache removes a whole class of staleness
// bugs (edit hooks.json → next tool call sees it, gated on approval).
[[nodiscard]] HooksFile load_hooks_file() {
    HooksFile out;
    const fs::path candidates[] = {
        fs::path{".agentty"} / "hooks.json",
        home_dir().empty() ? fs::path{} : home_dir() / ".agentty" / "hooks.json",
    };
    for (const auto& c : candidates) {
        if (c.empty()) continue;
        std::string raw = read_all(c, kMaxHooksFileBytes);
        if (raw.empty()) continue;
        std::error_code ec;
        auto abs = fs::weakly_canonical(c, ec);
        out.path = (ec ? fs::absolute(c, ec) : abs).string();
        out.hash = auth::sha256_hex(raw);
        json j = json::parse(raw, nullptr, /*throw=*/false);
        if (!j.is_object()) return out;   // ok=false: unparseable
        auto read_list = [&](const char* key, std::vector<HookEntry>& dst) {
            if (!j.contains(key) || !j[key].is_array()) return;
            for (const auto& e : j[key]) {
                if (!e.is_object()) continue;
                HookEntry h;
                h.match = e.value("match", std::string{});
                h.run   = e.value("run", std::string{});
                if (!h.run.empty()) dst.push_back(std::move(h));
            }
        };
        read_list("pre_tool",  out.pre_tool);
        read_list("post_tool", out.post_tool);
        out.ok = true;
        return out;
    }
    return out;   // no file
}

[[nodiscard]] fs::path approvals_path() {
    auto h = home_dir();
    if (h.empty()) return {};
    return h / ".agentty" / "hooks_approved.json";
}

// Hooks trust now rides on the SHARED content-hash primitive
// (scope::Approvals) instead of a bespoke store — one trust mechanism for
// every executable-from-untrusted-origin surface (plugins, hooks). The hash
// is the same value either way (scope::content_hash == auth::sha256_hex, and
// hf.hash is already sha256_hex(raw)), so nothing about the security property
// changes: approve a file's exact bytes; any byte change yields a new hash
// that isn't in the approved set, so it re-gates.
//
// The store leaf stays "hooks_approved.json" for continuity. scope::Approvals
// is a flat hash LIST; the pre-scope format was a {abs_path: hash} OBJECT.
// load_hook_approvals() reads either — an object is migrated in place by
// taking its hash VALUES — so nobody loses an approval across the upgrade.
constexpr char kHooksApprovalsLeaf[] = "hooks_approved.json";

[[nodiscard]] scope::Approvals load_hook_approvals() {
    // Fast path: the scope loader handles the new array form (and returns
    // empty on the legacy object form, which it can't parse as an array).
    scope::Approvals a = scope::load_approvals(kHooksApprovalsLeaf);
    if (!a.shas.empty()) return a;
    // Legacy {path: hash} object — migrate its values into the list.
    const auto p = approvals_path();
    std::string raw = read_all(p, kMaxHooksFileBytes);
    if (raw.empty()) return a;
    json j = json::parse(raw, nullptr, /*throw=*/false);
    if (j.is_object())
        for (auto it = j.begin(); it != j.end(); ++it)
            if (it->is_string()) a.approve(it->get<std::string>());
    return a;
}

[[nodiscard]] bool is_approved(const HooksFile& hf) {
    if (hf.path.empty() || hf.hash.empty()) return false;
    return load_hook_approvals().approved(hf.hash);
}

void store_approval(const HooksFile& hf) {
    if (hf.hash.empty()) return;
    scope::Approvals a = load_hook_approvals();   // includes any migrated legacy hashes
    a.approve(hf.hash);
    (void)scope::save_approvals(kHooksApprovalsLeaf, a);   // rewrites as the array form
}

[[nodiscard]] bool name_matches(const std::string& ere, std::string_view tool) {
    if (ere.empty()) return true;   // no match key ⇒ every tool
    try {
        std::regex re(ere, std::regex::extended | std::regex::nosubs);
        return std::regex_search(std::string{tool}, re);
    } catch (const std::regex_error&) {
        return false;   // bad pattern never matches (fail closed for pre)
    }
}

// Write the payload to a private temp file; returns its path ('' on error).
[[nodiscard]] std::string write_payload(const std::string& payload) {
    std::error_code ec;
    auto dir = fs::temp_directory_path(ec);
    if (ec) return {};
    auto p = dir / ("agentty_hook_" + std::to_string(::getpid()) + "_" +
                    std::to_string(reinterpret_cast<std::uintptr_t>(&payload)));
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    if (!f) return {};
    f << payload;
    f.close();
#ifndef _WIN32
    ::chmod(p.c_str(), 0600);
#endif
    return p.string();
}

// Run one hook command with the payload plumbed via env + file. Returns
// {exit_code, captured output}.
struct HookRun { int exit_code = 0; std::string output; };
[[nodiscard]] HookRun run_hook(const HookEntry& h, std::string_view event,
                               std::string_view tool,
                               const std::string& payload) {
    const std::string pf = write_payload(payload);
    // Env is inherited by the child; set + restore around the call. Tool
    // calls are serialised per-thread (run_hook fires on the tool's own
    // isolated thread) but env is process-global — guard with a mutex so
    // two concurrent tools can't interleave setenv windows.
    static std::mutex env_mu;
    std::lock_guard lk(env_mu);
#ifndef _WIN32
    ::setenv("AGENTTY_HOOK_EVENT", std::string{event}.c_str(), 1);
    ::setenv("AGENTTY_HOOK_TOOL",  std::string{tool}.c_str(), 1);
    if (!pf.empty()) ::setenv("AGENTTY_HOOK_PAYLOAD_FILE", pf.c_str(), 1);
#endif
    auto res = util::sandbox::run_shell_command(h.run, kMaxPayloadBytes,
                                                kHookTimeout);
#ifndef _WIN32
    ::unsetenv("AGENTTY_HOOK_EVENT");
    ::unsetenv("AGENTTY_HOOK_TOOL");
    ::unsetenv("AGENTTY_HOOK_PAYLOAD_FILE");
#endif
    if (!pf.empty()) {
        std::error_code ec;
        fs::remove(pf, ec);
    }
    return {res.exit_code, std::move(res.output)};
}

} // namespace

PreToolDecision run_pre_tool(std::string_view tool,
                             const std::string& args_json) {
    PreToolDecision d;
    if (hooks_disabled()) return d;
    HooksFile hf = load_hooks_file();
    if (!hf.ok || hf.pre_tool.empty()) return d;
    if (!is_approved(hf)) return d;   // unapproved file NEVER runs

    json payload = {{"event", "pre_tool"},
                    {"tool", std::string{tool}},
                    {"args", args_json}};
    const std::string body = payload.dump();
    for (const auto& h : hf.pre_tool) {
        if (!name_matches(h.match, tool)) continue;
        HookRun r = run_hook(h, "pre_tool", tool, body);
        if (r.exit_code != 0) {
            d.blocked = true;
            d.reason  = r.output.empty()
                ? ("blocked by pre_tool hook (`" + h.run + "` exited " +
                   std::to_string(r.exit_code) + ")")
                : r.output;
            return d;
        }
    }
    return d;
}

void run_post_tool(std::string_view tool, const std::string& args_json,
                   const std::string& result_text) {
    if (hooks_disabled()) return;
    HooksFile hf = load_hooks_file();
    if (!hf.ok || hf.post_tool.empty()) return;
    if (!is_approved(hf)) return;

    json payload = {{"event", "post_tool"},
                    {"tool", std::string{tool}},
                    {"args", args_json},
                    {"result",
                     result_text.size() > kMaxPayloadBytes
                         ? result_text.substr(0, kMaxPayloadBytes)
                         : result_text}};
    const std::string body = payload.dump();
    for (const auto& h : hf.post_tool) {
        if (!name_matches(h.match, tool)) continue;
        (void)run_hook(h, "post_tool", tool, body);   // fire-and-forget
    }
}

bool pending_approval() {
    if (hooks_disabled()) return false;
    HooksFile hf = load_hooks_file();
    return hf.ok && !is_approved(hf);
}

std::string active_file() {
    return load_hooks_file().path;
}

int cli(const std::string& verb) {
    HooksFile hf = load_hooks_file();
    if (verb == "list" || verb.empty()) {
        if (hf.path.empty()) {
            std::printf("no hooks file (.agentty/hooks.json or "
                        "~/.agentty/hooks.json)\n");
            return 0;
        }
        std::printf("hooks file: %s\n", hf.path.c_str());
        if (!hf.ok) {
            std::printf("  PARSE ERROR — file is not valid JSON\n");
            return 1;
        }
        std::printf("  approval:  %s\n",
                    is_approved(hf) ? "APPROVED"
                                    : "NOT APPROVED (hooks will not run — "
                                      "`agentty hooks approve`)");
        auto show = [](const char* ev, const std::vector<HookEntry>& v) {
            for (const auto& h : v)
                std::printf("  %-9s match=%-20s run=%s\n", ev,
                            h.match.empty() ? "(all)" : h.match.c_str(),
                            h.run.c_str());
        };
        show("pre_tool",  hf.pre_tool);
        show("post_tool", hf.post_tool);
        return 0;
    }
    if (verb == "approve") {
        if (hf.path.empty()) {
            std::fprintf(stderr, "agentty hooks approve: no hooks file found\n");
            return 1;
        }
        if (!hf.ok) {
            std::fprintf(stderr, "agentty hooks approve: %s is not valid "
                                 "JSON — fix it first\n", hf.path.c_str());
            return 1;
        }
        if (is_approved(hf)) {
            std::printf("already approved: %s\n", hf.path.c_str());
            return 0;
        }
        // Show the exact bytes being approved, then require explicit y.
        std::printf("── %s ──\n%s\n────────\n"
                    "These hooks run ARBITRARY SHELL automatically on "
                    "matching tool calls\n(sandboxed like the bash tool, "
                    "but still able to act inside your workspace).\n"
                    "Approve this exact file content? [y/N] ",
                    hf.path.c_str(),
                    read_all(hf.path, kMaxHooksFileBytes).c_str());
        std::fflush(stdout);
        std::string line;
        std::getline(std::cin, line);
        if (line != "y" && line != "Y" && line != "yes") {
            std::printf("not approved\n");
            return 1;
        }
        store_approval(hf);
        std::printf("approved (any byte change re-gates)\n");
        return 0;
    }
    std::fprintf(stderr, "usage: agentty hooks [list|approve]\n");
    return 2;
}

} // namespace agentty::tools::hooks
