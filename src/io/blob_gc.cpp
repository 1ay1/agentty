// agentty::blobs::gc — mark and sweep. See blob_gc.hpp for the design.

#include "agentty/io/blob_gc.hpp"

#include "agentty/io/blob_store.hpp"
#include "agentty/io/persistence.hpp"
#include "agentty/util/logx.hpp"

#include <nlohmann/json.hpp>

#include <fstream>
#include <string>
#include <string_view>
#include <unordered_set>

namespace agentty::blobs {

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {

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

GcStats collect_in(const fs::path& threads_dir, bool dry_run) {
    GcStats st;
    std::error_code ec;
    if (!fs::is_directory(threads_dir, ec)) return st;

    // ── MARK ──────────────────────────────────────────────────────────
    std::unordered_set<std::string> referenced;
    for (const auto& e : fs::directory_iterator(threads_dir, ec)) {
        if (!e.is_regular_file(ec)) continue;
        const auto p   = e.path();
        const auto ext = p.extension();
        if (ext != ".json" && ext != ".jsonl") continue;

        // index.json is our own picker cache and acp_sessions.json belongs
        // to the ACP server; neither is a thread and neither holds blob
        // references. Metadata sidecars (<id>.meta.json) DO get scanned:
        // they carry compaction summaries, which go through put_or_inline
        // and can therefore hold a blob reference.
        const auto name = p.filename().string();
        if (name == "index.json" || name.starts_with("acp_sessions")) continue;

        ++st.scanned_threads;
        if (!refs_from_file(p, referenced)) {
            ++st.unreadable;
            AGT_LOG(Persist, Warn, "blob_gc",
                    "cannot read {} — aborting sweep", p.string());
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
        if (e.is_regular_file(ec)) ++st.total_blobs;
    for (const auto& r : referenced)
        if (fs::exists(blob_dir / r, ec)) ++st.referenced;
    st.dangling = referenced.size() - st.referenced;

    if (st.unreadable > 0) {
        AGT_LOG(Persist, Error, "blob_gc",
                "{} unreadable thread file(s) — deleted nothing", st.unreadable);
        return st;   // ran == false
    }
    st.ran = true;

    // ── SWEEP ─────────────────────────────────────────────────────────
    for (const auto& e : fs::directory_iterator(blob_dir, ec)) {
        if (!e.is_regular_file(ec)) continue;
        const auto name = e.path().filename().string();
        if (referenced.count(name)) continue;

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

} // namespace agentty::blobs
