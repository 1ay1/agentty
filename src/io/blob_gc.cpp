// agentty::blobs::gc — mark and sweep. See blob_gc.hpp for the design.

#include "agentty/io/blob_gc.hpp"

#include "agentty/io/blob_store.hpp"
#include "agentty/io/persistence.hpp"
#include "agentty/util/logx.hpp"
#include "agentty/util/teardown.hpp"

#include <nlohmann/json.hpp>

#include <maya/runtime.hpp>

#include <atomic>
#include <chrono>
#include <fstream>
#include <mutex>
#include <stop_token>
#include <string>
#include <string_view>
#include <unordered_set>

namespace agentty::blobs {

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {

// Set by join_background_gc(). Checked between files in every walk so a
// sweep over thousands of threads stops promptly at shutdown.
std::atomic<bool> g_cancel{false};
[[nodiscard]] bool cancelled() noexcept { return g_cancel.load(std::memory_order_relaxed); }

constexpr std::string_view kStampName = ".gc-stamp";

// Every key under which a blob name can appear.
//
// Deliberately NOT a hardcoded list. The writer generates some of these
// dynamically — put_or_inline() writes `<field>_blob` for whatever field
// it is given, so `thinking_blob`, `signature_blob` and `text_blob` all
// exist and a new one appears the moment someone calls it with a new
// field. A fixed list would silently stop protecting those, and the
// symptom would be deleting a payload that IS referenced.
//
// So the rule is structural: any string value whose KEY is "blob" or ends
// in "_blob" is a reference. That matches how the writer produces them,
// so the two cannot drift.
[[nodiscard]] bool is_blob_key(std::string_view key) noexcept {
    return key == "blob" || key.ends_with("_blob");
}

// Walk a parsed document collecting every blob reference. Recursive
// because references live at several depths: message.images[].blob,
// message.tool_calls[].output_blob, message.thinking_blocks[].text_blob.
void collect_refs(const json& j, std::unordered_set<std::string>& out) {
    if (j.is_object()) {
        for (const auto& [k, v] : j.items()) {
            if (is_blob_key(k) && v.is_string()) {
                auto name = v.get<std::string>();
                if (!name.empty()) out.insert(std::move(name));
            } else {
                collect_refs(v, out);
            }
        }
    } else if (j.is_array()) {
        for (const auto& v : j) collect_refs(v, out);
    }
}

// Read one thread file's references. Returns false if the file exists but
// could not be understood — the caller treats that as "abort the sweep",
// because a thread whose references are unknown must not have its
// payloads deleted.
[[nodiscard]] bool refs_from_file(const fs::path& p,
                                  std::unordered_set<std::string>& out) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return false;

    if (p.extension() == ".jsonl") {
        // The log: one document per line. A single unparseable line is
        // tolerated here for the same reason range() tolerates it — a
        // torn final append — but it means this file's references are
        // incomplete, so it still fails the whole sweep.
        std::string line;
        bool ok = true;
        while (std::getline(in, line)) {
            if (line.empty()) continue;
            json j = json::parse(line, nullptr, /*allow_exceptions=*/false);
            if (j.is_discarded()) { ok = false; continue; }
            collect_refs(j, out);
        }
        return ok;
    }

    std::string buf{std::istreambuf_iterator<char>(in),
                    std::istreambuf_iterator<char>()};
    json j = json::parse(buf, nullptr, /*allow_exceptions=*/false);
    if (j.is_discarded()) return false;
    collect_refs(j, out);
    return true;
}

} // namespace

GcStats collect_in(const fs::path& threads_dir, bool dry_run,
                   std::chrono::seconds min_age) {
    GcStats st;
    std::error_code ec;
    if (!fs::is_directory(threads_dir, ec)) return st;

    // ── MARK ──────────────────────────────────────────────────────────
    std::unordered_set<std::string> referenced;
    {
        // Open the walk EXPLICITLY so a failure is a fact, not an absence.
        //
        // `directory_iterator(dir, ec)` yields nothing when it cannot open
        // the directory -- which is indistinguishable from "no threads" to
        // a range-for. That is the difference between "nothing references
        // these blobs" and "I could not find out", and here the second
        // silently becomes the first: `referenced` stays empty, `unreadable`
        // stays 0, and SWEEP then deletes EVERY blob as unreferenced.
        //
        // Count it as unreadable instead. The safety rule below already
        // says what to do with that -- report and delete nothing -- it was
        // just never told.
        std::error_code oec;
        fs::directory_iterator walk{threads_dir, oec};
        if (oec) {
            ++st.unreadable;
            AGT_LOG(Persist, Error, "blob_gc",
                    "cannot list {} ({}) — deleted nothing",
                    threads_dir.string(), oec.message());
            return st;   // ran == false
        }
        for (const auto& e : walk) {
            if (cancelled()) return st;      // ran == false: nothing deleted
            if (!e.is_regular_file(ec)) continue;
            const auto p   = e.path();
            const auto ext = p.extension();
            if (ext != ".json" && ext != ".jsonl") continue;

            // index.json is our own picker cache and acp_sessions.json
            // belongs to the ACP server; neither is a thread and neither
            // holds blob references. Metadata sidecars (<id>.meta.json) DO
            // get scanned: they carry compaction summaries, which go through
            // put_or_inline and can therefore hold a blob reference.
            const auto name = p.filename().string();
            if (name == "index.json" || name.starts_with("acp_sessions"))
                continue;

            ++st.scanned_threads;
            if (!refs_from_file(p, referenced)) {
                ++st.unreadable;
                AGT_LOG(Persist, Warn, "blob_gc",
                        "cannot read {} — aborting sweep", p.string());
            }
        }
    }

    // ── The safety rule ───────────────────────────────────────────────
    // One unreadable thread means one set of unknown references. Deleting
    // anything at that point risks destroying a payload that IS in use,
    // and an unreadable thread file is precisely when the user most needs
    // their payloads intact. Report and stop.
    const auto blob_dir = threads_dir / "blobs";
    if (!fs::is_directory(blob_dir, ec)) { st.ran = st.unreadable == 0; return st; }

    for (const auto& e : fs::directory_iterator(blob_dir, ec))
        if (e.is_regular_file(ec) && e.path().filename() != kStampName)
            ++st.total_blobs;
    for (const auto& r : referenced)
        if (fs::exists(blob_dir / r, ec)) ++st.referenced;
    st.dangling = referenced.size() - st.referenced;

    if (st.unreadable > 0) {
        AGT_LOG(Persist, Error, "blob_gc",
                "{} unreadable thread file(s) — deleted nothing", st.unreadable);
        return st;   // ran == false
    }
    st.ran = true;

    // ── SWEEP ─────────────────────────────────────────────────────────────────────
    const auto cutoff = fs::file_time_type::clock::now() - min_age;
    for (const auto& e : fs::directory_iterator(blob_dir, ec)) {
        if (cancelled()) break;              // partial sweep is safe: each delete stands alone
        if (!e.is_regular_file(ec)) continue;
        const auto name = e.path().filename().string();
        if (referenced.count(name)) continue;
        if (name == kStampName) continue;
        if (min_age.count() > 0) {
            std::error_code tec;
            const auto mt = e.last_write_time(tec);
            // Unknown age or too young: a save may be about to reference it
            // (or it's a writer's in-progress temp). Leave it.
            if (tec || mt > cutoff) continue;
        }

        const auto sz = e.file_size(ec);
        if (dry_run) {
            st.bytes_freed += ec ? 0u : sz;
            ++st.deleted;
            continue;
        }
        std::error_code rm;
        if (fs::remove(e.path(), rm)) {
            st.bytes_freed += ec ? 0u : sz;
            ++st.deleted;
        }
    }

    AGT_LOG(Persist, Info, "blob_gc",
            "{} {} orphan blob(s), {} KB, across {} threads",
            dry_run ? "would reclaim" : "reclaimed",
            st.deleted, st.bytes_freed / 1024, st.scanned_threads);
    return st;
}

GcStats collect(bool dry_run) {
    return collect_in(persistence::threads_dir(), dry_run);
}

std::optional<GcStats> collect_if_due() {
    using namespace std::chrono;
    const auto blob_dir = persistence::threads_dir() / "blobs";
    std::error_code ec;
    if (!fs::is_directory(blob_dir, ec)) return std::nullopt;
    const auto stamp = blob_dir / kStampName;
    const auto now = fs::file_time_type::clock::now();
    if (auto t = fs::last_write_time(stamp, ec); !ec && now - t < hours{24})
        return std::nullopt;
    // Claim the slot first so two instances starting together don't both
    // sweep; a failed sweep simply retries tomorrow.
    { std::ofstream touch(stamp, std::ios::trunc); }
    fs::last_write_time(stamp, now, ec);
    return collect_in(persistence::threads_dir(), /*dry_run=*/false, hours{24});
}

namespace {
// This subsystem's OWN pool, because join_background_gc() is a public promise
// that ITS sweep is done — and you cannot join one job out of a shared pool.
// That is the policy line: fire-and-forget with no join goes to
// util::run_background / run_isolated_detached; work with a named join owns a
// pool. A pool nobody posts to never spawns a thread, so this costs nothing
// until the sweep is scheduled.
//
// What this replaced: a mutex + condition variable + raw worker thread + stop
// flag, whose shutdown joined if it could and DETACHED if the walk was still
// running. The detach is the part that mattered — it let a sweep keep
// walking while the CRT destroyed the statics under it. pool::shutdown asks
// the job to stop, waits inside a bounded grace, and only then abandons; see
// its comment, which documents this exact failure from agentty's own symbol
// scan.
maya::pool& gc_pool() {
    // One worker: there is only ever one sweep.
    static maya::pool p{/*max_workers=*/1};
    return p;
}
constexpr auto kStartDelay = std::chrono::seconds(20);
std::atomic<bool> g_scheduled{false};
}  // namespace

void start_background_gc() {
    // Register the join where the work is created, so main() never has to know
    // this subsystem is threaded. gc_pool() is a process-lifetime
    // function-local static, so the callback cannot outlive its target and
    // needs no cancel().
    static std::once_flag registered;
    std::call_once(registered, [] {
        util::teardown::on_shutdown("blobs.gc", [] { join_background_gc(); });
    });

    // Once per process. Previously expressed as "is the thread joinable",
    // which the pool no longer exposes — and did not need to be a lock.
    if (g_scheduled.exchange(true, std::memory_order_relaxed)) return;

    // Isolated, not queued: the walk is unbounded in principle (a large blob
    // store on a slow disk), and it must not occupy a shared worker.
    gc_pool().post_isolated([](std::stop_token st) {
        // The delay means a process that exits at once never starts the walk
        // at all. delay_for returns true if we were asked to stop first — one
        // cancellation channel, the token, rather than a second stop flag that
        // someone has to remember to notify.
        if (maya::delay_for(st, kStartDelay)) return;
        // Bridge the token to the walk's own cancel flag, so a sweep already
        // in progress stops at its next check instead of running to
        // completion after shutdown has been requested.
        const std::stop_callback cancel_on_stop(st, [] {
            g_cancel.store(true, std::memory_order_relaxed);
        });
        try {
            (void)collect_if_due();
        } catch (const std::exception& e) {
            AGT_LOG(Persist, Warn, "blob_gc", "{}", e.what());
        } catch (...) {}
    });
}

void join_background_gc() noexcept {
    // Cancel first, then wait: setting the flag before the stop request means
    // a walk between checks sees it on this side of the grace rather than
    // burning the grace and being abandoned.
    g_cancel.store(true, std::memory_order_relaxed);
    (void)gc_pool().shutdown();   // requests stop, waits within the grace
}

} // namespace agentty::blobs
