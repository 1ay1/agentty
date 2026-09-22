// SPDX-License-Identifier: Apache-2.0
//
// update_ux_test — the self-update's VISIBLE behaviour, not its arithmetic.
//
// update_check_test already pins version_less / platform_asset / the cache
// rule. None of that is what a user experiences. What they experience is:
//
//   * a ~15 MB download that must not look like a hang, and
//   * the fact that a finished update has NOT actually taken effect yet —
//     the running process keeps the image it started with, so a restart is
//     still owed.
//
// Both were weak. The TUI called perform_update() with no progress callback
// at all (the shell path had always passed one), so the status line froze on
// "⬆ downloading agentty…" for the whole transfer. And on success the "⬆
// vX.Y.Z" chip was CLEARED, leaving a single status line as the only trace —
// which a busy session scrolls past in seconds, after which nothing on screen
// says an update is waiting.
//
// These drive the real meta_update reducer.

#include <chrono>
#include <string>

#include "agtest.hpp"

#include "agentty/runtime/app/deps.hpp"
#include "agentty/runtime/app/update/internal.hpp"
#include "agentty/runtime/model.hpp"
#include "agentty/runtime/msg.hpp"
#include "agentty/util/update.hpp"

namespace A = agentty;
namespace D = agentty::app::detail;

namespace {

// The reducer arms under test touch no dependency, but install_deps must
// have run for the Model to be usable.
void install_stub_deps() {
    A::app::install_deps(A::app::Deps{
        .stream        = [](A::provider::Request, A::provider::EventSink) {},
        .save_thread   = [](const A::Thread&) {},
        .delete_thread = [](const A::ThreadId&) {},
    });
}

A::Model updating_model(const char* to_version) {
    install_stub_deps();
    A::Model m;
    m.s.update_latest    = to_version;
    m.s.update_in_flight = true;
    m.s.status           = "⬆ downloading agentty v" + std::string{to_version} + "…";
    return m;
}

bool has(const std::string& hay, const char* needle) {
    return hay.find(needle) != std::string::npos;
}

} // namespace

TEST_CASE("update: finding a release starts the download without being asked") {
    install_stub_deps();
    A::Model m;

    auto [next, cmd] = D::meta_update(
        std::move(m),
        A::msg::MetaMsg{A::UpdateCheckDone{true, "9.9.9",
                                           "https://example/releases"}});

    CHECK(next.s.update_latest == "9.9.9");
    // The whole point: no palette trip, no keystroke. Either the download
    // is already running, or self_update_possible() refused this install
    // (package manager / nix / Windows) and we correctly did nothing.
    std::string why;
    if (A::update::self_update_possible(why)) {
        CHECK(next.s.update_in_flight);
    } else {
        CHECK(!next.s.update_in_flight);   // not ours to replace
    }
}

TEST_CASE("update: a staged update does not start a second download") {
    install_stub_deps();
    A::Model m;
    m.s.update_pending_restart = "9.9.9";   // already installed, awaiting restart

    auto [next, cmd] = D::meta_update(
        std::move(m),
        A::msg::MetaMsg{A::UpdateCheckDone{true, "9.9.9", "https://example"}});

    // Re-downloading a version already written to disk would overwrite the
    // staged binary for no gain, and on a slow link would do it repeatedly
    // as the hourly poll keeps finding the same release.
    CHECK(!next.s.update_in_flight);
}

TEST_CASE("update: an in-flight download is not restarted by another check") {
    install_stub_deps();
    A::Model m;
    m.s.update_in_flight = true;

    auto [next, cmd] = D::meta_update(
        std::move(m),
        A::msg::MetaMsg{A::UpdateCheckDone{true, "9.9.9", "https://example"}});

    CHECK(next.s.update_in_flight);   // still the ORIGINAL one
}

TEST_CASE("update: download progress reaches the status line") {
    auto m = updating_model("1.2.3");
    const std::string before = m.s.status;

    // 4 MiB of 16 MiB.
    auto [next, _] = D::meta_update(
        std::move(m),
        A::msg::MetaMsg{A::UpdateProgress{4u * 1024 * 1024,
                                               16u * 1024 * 1024}});

    CHECK(next.s.status != before);          // it MOVED — that is the point
    CHECK(has(next.s.status, "25%"));
    CHECK(has(next.s.status, "4.0"));
    CHECK(has(next.s.status, "16.0"));
    // Progress must not expire mid-download and leave a blank bar.
    CHECK(next.s.status_until == std::chrono::steady_clock::time_point{});
}

TEST_CASE("update: unknown content-length shows bytes, not a fake percentage") {
    auto m = updating_model("1.2.3");

    // total = 0 → the server sent no Content-Length.
    auto [next, _] = D::meta_update(
        std::move(m),
        A::msg::MetaMsg{A::UpdateProgress{2u * 1024 * 1024, 0}});

    CHECK(has(next.s.status, "2.0"));
    // Inventing "0%" or "100%" from nothing would be a lie about progress.
    CHECK(!has(next.s.status, "%"));
}

TEST_CASE("update: progress arriving after the download is ignored") {
    install_stub_deps();
    A::Model m;
    m.s.update_in_flight = false;            // already finished/failed
    m.s.status           = "✓ updated to v1.2.3 — restart agentty to use it";
    const std::string before = m.s.status;

    auto [next, _] = D::meta_update(
        std::move(m),
        A::msg::MetaMsg{A::UpdateProgress{1u * 1024 * 1024,
                                               4u * 1024 * 1024}});

    // A late in-flight callback must not overwrite the outcome with a
    // stale "downloading…" line.
    CHECK(next.s.status == before);
}

TEST_CASE("update: a finished update still asks for a restart, persistently") {
    auto m = updating_model("1.2.3");

    auto [next, _] = D::meta_update(
        std::move(m), A::msg::MetaMsg{A::UpdateApplied{true, "1.2.3"}});

    CHECK(!next.s.update_in_flight);
    // The "available" signal is spent…
    CHECK(next.s.update_latest.empty());
    // …but the RESTART is still owed, and says so in a field the status bar
    // renders every frame — not only in a line that scrolls away.
    CHECK(next.s.update_pending_restart == "1.2.3");
    CHECK(has(next.s.status, "restart"));
    CHECK(next.s.status_until == std::chrono::steady_clock::time_point{});   // sticky
}

TEST_CASE("update: a failed update leaves the offer in place") {
    auto m = updating_model("1.2.3");

    auto [next, _] = D::meta_update(
        std::move(m),
        A::msg::MetaMsg{A::UpdateApplied{false, "connection reset"}});

    CHECK(!next.s.update_in_flight);
    // Nothing was installed, so nothing is pending a restart. Claiming
    // otherwise would park a permanent chip over a failure.
    CHECK(next.s.update_pending_restart.empty());
    // And the update is still available to retry.
    CHECK(next.s.update_latest == "1.2.3");
    CHECK(has(next.s.status, "connection reset"));
}
