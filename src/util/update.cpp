// SPDX-License-Identifier: Apache-2.0
//
// update.cpp — self-update against the GitHub releases of 1ay1/agentty.
// See include/agentty/util/update.hpp for the design contract.

#include "agentty/util/update.hpp"

#include "agentty/auth/auth.hpp"    // auth::config_dir()
#include "agentty/io/http.hpp"
#include "agentty/util/dbglog.hpp"

#include <nlohmann/json.hpp>

#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

#ifndef _WIN32
#include <unistd.h>     // readlink
#include <sys/stat.h>   // chmod
#endif

#ifndef AGENTTY_VERSION
#define AGENTTY_VERSION "0.0.0"
#endif

namespace agentty::update {

namespace fs = std::filesystem;
using nlohmann::json;

namespace {

constexpr const char* kRepo = "1ay1/agentty";
constexpr auto kCacheTtl = std::chrono::hours{24};

fs::path cache_path() { return auth::config_dir() / "update_check.json"; }

// Where is the running binary?
fs::path self_path() {
#ifdef _WIN32
    return {};   // Win self-replace not wired yet (MSI is the channel)
#else
    char buf[4096];
    ssize_t n = ::readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n <= 0) {
        // macOS: no /proc — _NSGetExecutablePath would go here; for now the
        // PATH-resolved argv[0] fallback is handled by the caller messaging.
        return {};
    }
    buf[n] = '\0';
    return fs::path{buf};
#endif
}

struct HttpResult { int status = 0; std::string body; std::string err; std::string location; };

HttpResult https_get(std::string_view host, std::string_view path,
                     std::size_t max_bytes,
                     const std::function<void(std::size_t, std::size_t)>& progress = {}) {
    HttpResult r;
    http::Request req;
    req.method = http::HttpMethod::Get;
    req.host   = std::string{host};
    req.port   = 443;
    req.path   = std::string{path};
    req.headers = {
        {"accept", "application/vnd.github+json"},
        {"user-agent", "agentty-updater/" AGENTTY_VERSION},
    };
    req.max_body_bytes = max_bytes;
    http::Timeouts tos;
    tos.connect = std::chrono::milliseconds(8'000);
    tos.total   = std::chrono::milliseconds(120'000);
    auto resp = http::default_client().send(req, tos);
    if (!resp) { r.err = resp.error().render(); return r; }
    r.status = resp->status;
    r.body   = std::move(resp->body);
    for (const auto& h : resp->headers) {
        std::string name = h.name;
        for (auto& c : name) c = static_cast<char>(std::tolower((unsigned char)c));
        if (name == "location") { r.location = h.value; break; }
    }
    if (progress && r.status == 200) progress(r.body.size(), r.body.size());
    return r;
}

// Split "https://host/path" → {host, path}. Empty host on parse failure.
std::pair<std::string, std::string> split_url(const std::string& url) {
    auto scheme = url.find("://");
    if (scheme == std::string::npos) return {};
    auto host_start = scheme + 3;
    auto slash = url.find('/', host_start);
    if (slash == std::string::npos) return {url.substr(host_start), "/"};
    return {url.substr(host_start, slash - host_start), url.substr(slash)};
}

void write_cache(const CheckResult& c) {
    json j = {
        {"checked_at", std::chrono::duration_cast<std::chrono::seconds>(
                           std::chrono::system_clock::now().time_since_epoch())
                           .count()},
        {"current", c.current},
        {"latest", c.latest},
        {"url", c.url},
        {"update_available", c.update_available},
    };
    std::error_code ec;
    fs::create_directories(cache_path().parent_path(), ec);
    std::ofstream f(cache_path(), std::ios::trunc);
    if (f) f << j.dump(2);
}

} // namespace

bool version_less(const std::string& a, const std::string& b) {
    auto parse = [](const std::string& s, int out[3]) {
        out[0] = out[1] = out[2] = 0;
        std::size_t i = 0;
        if (i < s.size() && (s[i] == 'v' || s[i] == 'V')) ++i;
        for (int k = 0; k < 3; ++k) {
            int v = 0;
            bool any = false;
            while (i < s.size() && std::isdigit((unsigned char)s[i])) {
                v = v * 10 + (s[i] - '0');
                ++i;
                any = true;
            }
            out[k] = any ? v : 0;
            if (i < s.size() && s[i] == '.') ++i;
            else break;
        }
    };
    int va[3], vb[3];
    parse(a, va);
    parse(b, vb);
    for (int k = 0; k < 3; ++k) {
        if (va[k] != vb[k]) return va[k] < vb[k];
    }
    return false;
}

std::string current_version() { return AGENTTY_VERSION; }

std::string platform_asset() {
#if defined(_WIN32)
    return "agentty-windows-x86_64.exe";
#elif defined(__APPLE__)
#  if defined(__aarch64__) || defined(__arm64__)
    return "agentty-macos-arm64";
#  else
    return "agentty-macos-x86_64";
#  endif
#elif defined(__linux__)
#  if defined(__aarch64__)
    return "agentty-linux-aarch64";
#  elif defined(__i386__)
    return "agentty-linux-i686";
#  else
    return "agentty-linux-x86_64";
#  endif
#else
    return {};
#endif
}

std::optional<CheckResult> cached_check() {
    std::ifstream f(cache_path());
    if (!f) return std::nullopt;
    try {
        json j = json::parse(f);
        auto age = std::chrono::system_clock::now().time_since_epoch()
                 - std::chrono::seconds{j.value("checked_at", std::int64_t{0})};
        if (age > kCacheTtl) return std::nullopt;
        CheckResult c;
        c.current          = j.value("current", std::string{});
        c.latest           = j.value("latest", std::string{});
        c.url              = j.value("url", std::string{});
        c.update_available = j.value("update_available", false);
        // A cache written by an OLDER binary is stale the moment the binary
        // changed (post-update relaunch must not re-announce the old delta).
        if (c.current != current_version()) return std::nullopt;
        return c;
    } catch (...) {
        return std::nullopt;
    }
}

CheckResult check_latest(bool force) {
    if (!force) {
        if (auto c = cached_check()) return *c;
    }
    CheckResult c;
    c.current = current_version();

    auto r = https_get("api.github.com",
                       std::string{"/repos/"} + kRepo + "/releases/latest",
                       1 * 1024 * 1024);
    if (!r.err.empty()) { c.error = r.err; return c; }
    if (r.status != 200) {
        c.error = "GitHub API HTTP " + std::to_string(r.status);
        return c;
    }
    try {
        json j = json::parse(r.body);
        std::string tag = j.value("tag_name", std::string{});
        c.latest = tag;
        if (!tag.empty() && tag.front() == 'v') c.latest = tag.substr(1);
        c.url = j.value("html_url",
                        std::string{"https://github.com/"} + kRepo + "/releases");
        c.update_available = version_less(c.current, c.latest);
        write_cache(c);
    } catch (const std::exception& e) {
        c.error = std::string{"release JSON parse failed: "} + e.what();
    }
    return c;
}

bool self_update_possible(std::string& reason) {
#ifdef _WIN32
    reason = "self-update is not supported on Windows yet — download the "
             "installer from https://github.com/1ay1/agentty/releases";
    return false;
#else
    fs::path self = self_path();
    if (self.empty()) {
        reason = "could not resolve the running binary's path";
        return false;
    }
    std::string p = self.string();
    // Package-manager territory: overwriting under /usr (except /usr/local
    // hand-installs) fights rpm/pacman/portage — tell the user to use them.
    if (p.starts_with("/usr/") && !p.starts_with("/usr/local/")) {
        reason = "installed by a package manager (" + p +
                 ") — update with your package manager instead";
        return false;
    }
    if (p.find("/nix/store/") != std::string::npos) {
        reason = "installed via nix — update through your nix configuration";
        return false;
    }
    // Writable check on the parent dir (the atomic rename target).
    std::error_code ec;
    auto dir = self.parent_path();
    auto probe = dir / ".agentty_update_probe";
    std::ofstream f(probe);
    if (!f) {
        reason = "no write permission for " + dir.string() +
                 " — re-run with appropriate permissions";
        return false;
    }
    f.close();
    fs::remove(probe, ec);
    return true;
#endif
}

std::string perform_update(
    const std::string& tag,
    const std::function<void(std::size_t, std::size_t)>& progress) {
    std::string reason;
    if (!self_update_possible(reason)) return reason;

    const std::string asset = platform_asset();
    if (asset.empty()) return "no prebuilt binary for this platform";

    const std::string vtag = tag.starts_with("v") ? tag : "v" + tag;

    // GitHub's stable download URL 302s to a CDN; follow up to 4 hops.
    std::string host = "github.com";
    std::string path = "/" + std::string{kRepo} + "/releases/download/" +
                       vtag + "/" + asset;
    HttpResult r;
    for (int hop = 0; hop < 4; ++hop) {
        r = https_get(host, path, 512ull * 1024 * 1024, progress);
        if (!r.err.empty()) return "download failed: " + r.err;
        if (r.status == 301 || r.status == 302 || r.status == 307
            || r.status == 308) {
            auto [h, p] = split_url(r.location);
            if (h.empty()) return "download failed: bad redirect";
            host = std::move(h);
            path = std::move(p);
            continue;
        }
        break;
    }
    if (r.status != 200)
        return "download failed: HTTP " + std::to_string(r.status) +
               " for " + asset + " (release " + vtag + ")";
    if (r.body.size() < 1024 * 1024)
        return "downloaded binary suspiciously small (" +
               std::to_string(r.body.size()) + " bytes) — aborting";
    // ELF/Mach-O magic sanity: never install an HTML error page as a binary.
#ifndef _WIN32
    const bool looks_binary =
        (r.body.size() > 4)
        && ((r.body[0] == 0x7f && r.body[1] == 'E' && r.body[2] == 'L'
             && r.body[3] == 'F')                                    // ELF
            || ((unsigned char)r.body[0] == 0xcf
                && (unsigned char)r.body[1] == 0xfa)                 // Mach-O 64
            || ((unsigned char)r.body[0] == 0xca
                && (unsigned char)r.body[1] == 0xfe));               // Mach-O fat
    if (!looks_binary)
        return "downloaded file is not an executable — aborting";
#endif

    fs::path self = self_path();
    fs::path tmp  = self;
    tmp += ".update.tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) return "cannot write " + tmp.string();
        f.write(r.body.data(), static_cast<std::streamsize>(r.body.size()));
        if (!f) {
            std::error_code ec;
            fs::remove(tmp, ec);
            return "short write to " + tmp.string();
        }
    }
#ifndef _WIN32
    ::chmod(tmp.c_str(), 0755);
#endif
    // Atomic swap: rename(2) over the live binary. The running process keeps
    // executing its old (unlinked) image — next launch gets the new one.
    std::error_code ec;
    fs::rename(tmp, self, ec);
    if (ec) {
        fs::remove(tmp, ec);
        return "could not replace " + self.string() + ": " + ec.message();
    }
    // Invalidate the check cache so the fresh binary re-checks cleanly.
    fs::remove(cache_path(), ec);
    return {};
}

} // namespace agentty::update
