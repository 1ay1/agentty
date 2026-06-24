#include "agentty/runtime/view/thread/turn/agent_timeline/agent_timeline.hpp"

#include <list>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "agentty/runtime/view/helpers.hpp"
#include "agentty/runtime/view/palette.hpp"
#include "agentty/runtime/view/thread/turn/agent_timeline/tool_args.hpp"
#include "agentty/runtime/view/thread/turn/agent_timeline/tool_body_preview.hpp"
#include "agentty/runtime/view/thread/turn/agent_timeline/tool_helpers.hpp"

namespace agentty::ui {
namespace {

// ── Settled-tool body cache ───────────────────────────────────────────
//
// tool_body_preview_config() is O(body): it parses the tool's JSON args
// (safe_arg over the full `content`), materializes output(), and for an
// Edit scans the diff body for the ```diff fence + substr's the hunk
// text. For a write/edit with thousands of lines that's ~tens of ms.
//
// In an edit/write-heavy turn the in-flight assistant run carries MANY
// already-terminal cards (each sub-turn settles before the next
// continuation arrives). build_live_tail gives the whole in-flight run
// NO hash_id (the latest tool is still pending), so the host rebuilds
// every settled card's body EVERY frame — O(Σ body sizes) per frame at
// 10-60 fps. That is the climbing CPU as the turn grows.
//
// A terminal tool's body bytes are immutable: status is fixed,
// output().size() is fixed. So we memoize the built Config under the
// EXACT content-address the per-event hash_id already trusts —
// (id, status.index(), output().size(), highlight_lines signature).
// On hit we copy the cached Config (a plain string/vector copy) instead
// of re-parsing JSON + re-scanning the diff. Emitted bytes are
// byte-identical to a fresh build, so the freeze handoff (freeze_range
// stamps the same assistant_run_hash_id) stays a pure maya cache hit —
// no scrollback-corruption surface.
//
// Single-threaded by construction (runtime serializes update+view on one
// thread), so a thread_local store needs no locking. Bounded LRU so it
// can't grow with session length; settled cards that scroll out get
// frozen into m.ui.frozen and never query this again, so their entries
// fall out under LRU pressure naturally.
class BodyConfigCache {
public:
    static constexpr std::size_t kCap = 512;

    // Returns the cached Config for `key`, or nullptr on miss.
    std::shared_ptr<const maya::ToolBodyPreview::Config> get(const std::string& key) {
        auto it = map_.find(key);
        if (it == map_.end()) return nullptr;
        lru_.splice(lru_.begin(), lru_, it->second.lru_it);
        return it->second.cfg;
    }

    void put(const std::string& key,
             std::shared_ptr<const maya::ToolBodyPreview::Config> cfg) {
        auto it = map_.find(key);
        if (it != map_.end()) {
            it->second.cfg = std::move(cfg);
            lru_.splice(lru_.begin(), lru_, it->second.lru_it);
            return;
        }
        lru_.push_front(key);
        map_.emplace(key, Entry{std::move(cfg), lru_.begin()});
        while (map_.size() > kCap) {
            map_.erase(lru_.back());
            lru_.pop_back();
        }
    }

private:
    struct Entry {
        std::shared_ptr<const maya::ToolBodyPreview::Config> cfg;
        std::list<std::string>::iterator                     lru_it;
    };
    std::unordered_map<std::string, Entry> map_;
    std::list<std::string>                 lru_;
};

thread_local BodyConfigCache g_body_cache;

// ── Settled-panel ELEMENT cache ───────────────────────────────────────
//
// One tier above the body cache. The body cache memoizes each terminal
// tool's O(body) ToolBodyPreview::Config; THIS cache memoizes the WHOLE
// built panel Element for an all-terminal batch. A long in-flight
// autopilot turn accumulates many settled sub-turn panels in the live
// tail; the run has no hash_id while the tail streams, so without this
// the host re-ran agent_timeline_config (grep scan, per-event detail +
// CacheIdBuilder, footer/title assembly) AND maya re-laid-out the panel
// for every settled card on every frame — O(Σ batches) per frame, the
// linear lag the o1_probe "in-flight run, N accumulated edits" table
// shows. On hit we copy the prebuilt Element (a variant copy whose
// committed sub-trees still carry their per-event hash_ids, so maya keeps
// blitting their cells) instead of rebuilding anything.
//
// Same LRU + thread_local single-threaded discipline as g_body_cache.
class PanelElementCache {
public:
    // Sized to cover a very long single in-flight autopilot turn without
    // evicting panels still on screen. Entries are lightweight Element
    // handles (the heavy body cells live once in maya's component cache,
    // keyed by the per-event/panel hash_ids), so a high cap is cheap.
    // Below this, a 300-edit turn thrashed the cache (evicting panels
    // still in the live tail) and per-frame cost climbed again — the
    // 160→320 knee in the o1_probe scaling table.
    static constexpr std::size_t kCap = 512;

    const maya::Element* get(const std::string& key) {
        auto it = map_.find(key);
        if (it == map_.end()) return nullptr;
        lru_.splice(lru_.begin(), lru_, it->second.lru_it);
        return &it->second.el;
    }

    void put(const std::string& key, maya::Element el) {
        auto it = map_.find(key);
        if (it != map_.end()) {
            it->second.el = std::move(el);
            lru_.splice(lru_.begin(), lru_, it->second.lru_it);
            return;
        }
        lru_.push_front(key);
        map_.emplace(key, Entry{std::move(el), lru_.begin()});
        while (map_.size() > kCap) {
            map_.erase(lru_.back());
            lru_.pop_back();
        }
    }

private:
    struct Entry {
        maya::Element                    el;
        std::list<std::string>::iterator lru_it;
    };
    std::unordered_map<std::string, Entry> map_;
    std::list<std::string>                 lru_;
};

thread_local PanelElementCache g_panel_cache;

} // namespace

maya::AgentTimeline::Config agent_timeline_config(std::span<const ToolUse> tool_calls,
                                                  int spinner_frame,
                                                  maya::Color rail_color) {
    int total = static_cast<int>(tool_calls.size());
    int done  = 0;
    float total_elapsed = 0.0f;
    int running_idx = -1;

    std::vector<std::pair<std::string, int>> cat_counts;
    auto bump_cat = [&](const std::string& cat) {
        for (auto& [k, n] : cat_counts) if (k == cat) { ++n; return; }
        cat_counts.emplace_back(cat, 1);
    };

    for (std::size_t i = 0; i < tool_calls.size(); ++i) {
        const auto& tc = tool_calls[i];
        if (tc.is_terminal()) {
            ++done;
            total_elapsed += tool_elapsed(tc);
        }
        if (running_idx < 0 && (tc.is_running() || tc.is_approved()))
            running_idx = static_cast<int>(i);
        bump_cat(std::string{tool_category_label(tc.name.value)});
    }

    // Cross-tool semantics: scan completed Greps once up-front and build
    // a `path → {line numbers}` index. Subsequent Read/find_definition
    // tools that open any of those paths inherit the grep hits as
    // `highlight_lines`, anchoring the user's eye on lines the assistant
    // flagged earlier in the same turn instead of forcing a re-scan.
    // Mirrors agent_session.cpp's grep_hits → FileRead wiring in maya.
    const GrepHits grep_hits = collect_grep_hits(tool_calls);

    // Signature of the grep-hits index, folded into every terminal tool's
    // body-cache key. grep_hits feed a Read/find_definition body's
    // `highlight_lines`, which change the rendered HEIGHT without changing
    // output().size() — so a Read that settled before a same-path Grep
    // landed must NOT serve a stale (no-highlight) cached body once the
    // Grep indexes its path. Keying on the whole index is conservative
    // (any grep change re-mints all entries) but grep hits are stable
    // once the Greps settle, so the steady state stays a permanent hit.
    // Cheap to build: just the line numbers, no body bytes.
    std::string grep_sig;
    {
        // Deterministic order: unordered_map iteration order is unstable,
        // but we only need the signature to CHANGE when the contents do,
        // and to be identical across frames with identical contents —
        // which holds because the map is rebuilt identically each frame.
        for (const auto& [path, lines] : grep_hits) {
            grep_sig += path;
            grep_sig.push_back(':');
            for (int ln : lines) {
                grep_sig += std::to_string(ln);
                grep_sig.push_back(',');
            }
            grep_sig.push_back(';');
        }
    }

    maya::AgentTimeline::Config cfg;
    cfg.frame = spinner_frame;

    // ── Stats. Pick a representative color per category so the badge
    //    matches the per-event tree glyph color downstream.
    for (const auto& [cat, n] : cat_counts) {
        maya::Color cc = (cat == "mutate")  ? accent
                       : (cat == "execute") ? success
                       : (cat == "plan")    ? warn
                       : (cat == "vcs")     ? highlight
                                            : info;
        cfg.stats.push_back({cat, n, cc});
    }

    // ── Events.
    cfg.events.reserve(tool_calls.size());
    for (const auto& tc : tool_calls) {
        std::string detail = tool_timeline_detail(tc);
        if (detail.empty()) {
            detail = tc.is_running()  ? std::string{"running\xe2\x80\xa6"}
                   : tc.is_pending()  ? std::string{"queued\xe2\x80\xa6"}
                   : tc.is_approved() ? std::string{"approved\xe2\x80\xa6"}
                                      : std::string{"\xe2\x80\xa6"};
        }
        // Per-event hash_id on every TERMINAL tool event — live tail
        // AND frozen snapshot. Once a tool is terminal its bytes are
        // immutable (status fixed, output().size() fixed), so the cache
        // key is content-addressed and stable across every subsequent
        // frame: maya blits the event's (header + body) sub-tree from
        // its component cache instead of re-laying-out ToolBodyPreview's
        // per-line row Elements. For a settled write/edit with hundreds
        // of lines that collapses hundreds of flex rows into one cached
        // blit — the dominant per-frame cost of a tall settled card
        // sitting in an in-flight assistant run.
        //
        // Why this is safe in the live tail (the historical concern):
        // the maya prev_cells desync that originally caused live cards
        // to collapse to their last line and lose the left border is
        // fixed in maya HEAD (card-border tests 5/5). The freeze handoff
        // is also a pure cache hit: freeze_range builds the same Element
        // sub-tree under the same hash_id, so maya's cache entry survives
        // the live→frozen transition with zero re-paint.
        //
        // Why this is needed: an in-flight assistant run can contain
        // many already-terminal write/edit/read cards (each sub-turn
        // settles before the next continuation arrives). Without a key,
        // every frame rebuilds and re-lays out every tall body for the
        // entire run — ~21 ms/frame for a 3000-line read, multiplied
        // by every settled card in the run. Over ssh that turns into
        // visible lag as the turn grows.
        //
        // Body config FIRST — its grep-derived `highlight_lines` change
        // the rendered HEIGHT (FileRead prepends a `▸ matches: …` summary
        // row when non-empty) WITHOUT changing output().size(). A Read that
        // settled before a same-path Grep landed is cached with no
        // highlight; once the Grep's hits index the path, the body grows
        // one row but the id+status+size key is unchanged — maya would
        // blit the stale (shorter) cells into the taller reserved slot,
        // shifting every row below and bleeding stale cells (the
        // screenshot corruption). Fold the highlight signature into the
        // key so the height change mints a fresh entry.
        // Body build cache (terminal tools only). A terminal tool's body
        // bytes are immutable, so we memoize the O(body) Config build
        // under a content-address that captures every input the build
        // depends on: id + status + output size + the grep-hits signature
        // (which feeds Read/find_definition highlight_lines). On hit we
        // copy the cached Config instead of re-parsing JSON + re-scanning
        // the diff — turning O(Σ body sizes)/frame on a long in-flight run
        // into a cheap copy per settled card. Emitted bytes are identical
        // to a fresh build, so the freeze handoff stays a pure maya hit.
        maya::ToolBodyPreview::Config body;
        std::string body_key;
        const bool cacheable = tc.is_terminal();
        if (cacheable) {
            body_key.reserve(tc.id.value.size() + grep_sig.size() + 48);
            body_key += "t:";
            body_key += tc.id.value;
            body_key.push_back('|');
            body_key += std::to_string(tc.status.index());
            body_key.push_back('|');
            body_key += std::to_string(tc.output().size());
            body_key.push_back('|');
            body_key += grep_sig;
            if (auto hit = g_body_cache.get(body_key)) {
                body = *hit;  // copy the cached (immutable) build
            } else {
                body = tool_body_preview_config(tc, &grep_hits);
                g_body_cache.put(
                    body_key,
                    std::make_shared<const maya::ToolBodyPreview::Config>(body));
            }
        } else {
            body = tool_body_preview_config(tc, &grep_hits);
        }

        // Key: tool-call id + status + output size + body-height inputs
        // that aren't implied by output().size() (highlight_lines).
        // Permanent cache hit once terminal AND its highlight set is
        // stable; running/pending events stay un-keyed (their body still
        // mutates each frame and would alias a stale blit).
        maya::CacheId event_hash_id;
        if (tc.is_terminal()) {
            maya::CacheIdBuilder kb;
            kb.add(std::string_view{"agentty.tool_event"})
              .add(std::string_view{tc.id.value})
              .add(static_cast<std::uint64_t>(tc.status.index()))
              .add(static_cast<std::uint64_t>(tc.output().size()))
              .add(static_cast<std::uint64_t>(body.highlight_lines.size()));
            for (int hl : body.highlight_lines)
                kb.add(static_cast<std::uint64_t>(hl));
            event_hash_id = kb.build();
        }

        cfg.events.push_back({
            .name            = tool_display_name(tc.name.value),
            .detail          = std::move(detail),
            // Live elapsed for running/pending too — keeps the row's
            // right-edge duration cell present from the moment the
            // event renders, so the row doesn't horizontally snap
            // when the tool flips to terminal. tool_elapsed() uses
            // steady_clock::now() when finished_at is unset, which
            // is exactly the live counter we want.
            .elapsed_seconds = tool_elapsed(tc),
            .category_color  = tool_category_color(tc.name.value),
            .status          = tool_event_status(tc),
            .body            = std::move(body),
            .hash_id         = event_hash_id,
        });
    }

    // ── Footer. Present for the entire lifetime of the panel (live and
    //    settled) so the panel's row count doesn't grow by 1 when the
    //    last tool transitions to done. Height stability across that
    //    transition is what keeps panels straddling the scrollback seam
    //    from leaving rail / border fragments stranded: maya's row diff
    //    handles in-place row mutations fine, but a height delta on a
    //    panel that's already partially in native scrollback can't
    //    rewrite the rows that scrolled off.
    if (total > 0) {
        int failed = 0, rejected = 0;
        for (const auto& tc : tool_calls) {
            if (tc.is_failed())   ++failed;
            if (tc.is_rejected()) ++rejected;
        }
        const bool all_done = (done == total);
        maya::AgentTimelineFooter f;
        if (all_done) {
            f.glyph = "\xe2\x9c\x93";   // ✓
            f.text  = "done";
            f.color = success;
            if (failed > 0) {
                f.glyph = "\xe2\x9c\x97";           // ✗
                f.text  = std::to_string(failed) + " failed";
                f.color = danger;
            } else if (rejected > 0) {
                f.glyph = "\xe2\x8a\x98";           // ⊘
                f.text  = std::to_string(rejected) + " rejected";
                f.color = warn;
            }
        } else {
            f.glyph = "\xe2\x97\x8f";   // ●
            f.text  = "running";
            f.color = muted;
        }
        f.summary = std::to_string(done) + "/" + std::to_string(total)
                  + (total == 1 ? " action   " : " actions   ")
                  + format_duration_compact(total_elapsed);
        cfg.footer = std::move(f);
    }

    // ── Title and border. Left side: "ACTIONS · done/total". Right side
    //    (border-text-end): currently-running tool name while in flight,
    //    or the total elapsed once settled — splitting the two pins the
    //    elapsed to the right edge instead of leaving it left-glued to
    //    the action count.
    std::string title = " " + small_caps("Actions") + "  \xc2\xb7  "
                      + std::to_string(done) + "/" + std::to_string(total) + " ";
    std::string title_end;
    if (running_idx >= 0) {
        title_end = " " + tool_display_name(
            tool_calls[static_cast<std::size_t>(running_idx)].name.value) + " ";
    } else if (done == total && total > 0) {
        title_end = " " + format_duration_compact(total_elapsed) + " ";
    }

    bool all_done = (done == total && total > 0);
    cfg.title        = std::move(title);
    cfg.title_end    = std::move(title_end);
    cfg.border_color = all_done ? muted : rail_color;

    // Panel-level cache key for an ALL-TERMINAL batch. Settled tool bytes
    // are immutable and no chrome animates (every elapsed is final, no
    // spinner), so maya can blit the whole panel's cells across frames
    // instead of re-laying-out its rows. Content-address every input the
    // panel shape depends on: tool count + rail color + each tool's id,
    // status, output size, and render key. Leave empty when any tool is
    // still in-flight so the spinner/elapsed keep animating. The freeze
    // handoff stays a pure cache hit because freeze_range builds the same
    // batch under the same key.
    {
        bool all_terminal = total > 0;
        for (const auto& tc : tool_calls)
            if (!tc.is_terminal()) { all_terminal = false; break; }
        if (all_terminal) {
            maya::CacheIdBuilder kb;
            kb.add(std::string_view{"agentty.tool_panel"})
              .add(static_cast<std::uint64_t>(tool_calls.size()))
              .add(static_cast<std::uint64_t>(rail_color.kind()))
              .add(static_cast<std::uint64_t>(rail_color.r()))
              .add(static_cast<std::uint64_t>(rail_color.g()))
              .add(static_cast<std::uint64_t>(rail_color.b()));
            for (const auto& tc : tool_calls) {
                kb.add(std::string_view{tc.id.value})
                  .add(static_cast<std::uint64_t>(tc.status.index()))
                  .add(static_cast<std::uint64_t>(tc.output().size()))
                  .add(tc.compute_render_key());
            }
            cfg.hash_id = kb.build();
        }
    }
    return cfg;
}

maya::Element agent_timeline_element(std::span<const ToolUse> tool_calls,
                                     int spinner_frame,
                                     maya::Color rail_color) {
    // An all-terminal batch is byte-stable: every tool's status, output,
    // and args are fixed, and no spinner/elapsed counter animates (all
    // durations are final). So the WHOLE built Element is content-addressed
    // and identical across every later frame. A non-terminal batch carries
    // a running/pending tool (live elapsed counter, animated spinner via
    // `frame`) and MUST rebuild each frame — fall straight through.
    bool all_terminal = !tool_calls.empty();
    for (const auto& tc : tool_calls) {
        if (!tc.is_terminal()) { all_terminal = false; break; }
    }
    if (!all_terminal) {
        return maya::AgentTimeline{
            agent_timeline_config(tool_calls, spinner_frame, rail_color)}.build();
    }

    // Content-address: tool count + rail color + each tool's id, status,
    // output size, and render key (which folds args_streaming / progress /
    // output sizes — every input the panel body depends on). spinner_frame
    // is deliberately EXCLUDED: a settled panel has no animated glyph, so
    // the frame index doesn't affect its bytes; including it would defeat
    // the cache (the index ticks every frame).
    std::string key;
    key.reserve(tool_calls.size() * 40 + 16);
    key += "p|";
    key += std::to_string(static_cast<unsigned>(rail_color.kind()));
    key.push_back('.');
    key += std::to_string(rail_color.r());
    key.push_back('.');
    key += std::to_string(rail_color.g());
    key.push_back('.');
    key += std::to_string(rail_color.b());
    key.push_back('|');
    key += std::to_string(tool_calls.size());
    for (const auto& tc : tool_calls) {
        key.push_back('|');
        key += tc.id.value;
        key.push_back(':');
        key += std::to_string(tc.status.index());
        key.push_back(':');
        key += std::to_string(tc.output().size());
        key.push_back(':');
        key += std::to_string(tc.compute_render_key());
    }

    if (const maya::Element* hit = g_panel_cache.get(key))
        return *hit;   // copy the prebuilt, byte-stable panel Element

    maya::Element el = maya::AgentTimeline{
        agent_timeline_config(tool_calls, spinner_frame, rail_color)}.build();
    g_panel_cache.put(key, el);
    return el;
}

} // namespace agentty::ui
