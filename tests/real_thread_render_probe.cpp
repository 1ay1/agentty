// real_thread_render_probe — load a REAL thread off disk and render it.
//
// Not a unit test: a crash probe. Point it at a thread file and it walks
// the exact path the TUI walks when you pick a thread out of the picker —
// load → rehydrate_frozen → conversation_config → render_tree — at a
// couple of widths, so a stack overflow or a dangling view in the render
// walk reproduces here instead of only in the user's terminal.
//
//   ./build/real_thread_render_probe ~/.agentty/threads/<id>.json
//
// Exit 0 = rendered clean. A signal = the bug, with a usable backtrace.

#include <agentty/domain/session.hpp>
#include <agentty/io/persistence.hpp>
#include <agentty/io/thread_log.hpp>
#include <agentty/runtime/model.hpp>
#include <agentty/runtime/view/thread/conversation.hpp>
#include <agentty/runtime/app/update/internal.hpp>

#include <maya/render/renderer.hpp>
#include <maya/widget/conversation.hpp>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <nlohmann/json.hpp>

using namespace agentty;

// Deepest nesting level in the Element tree. paint_element / layout /
// build_layout_tree all recurse once per level, so this number times the
// per-frame stack cost is what has to fit in the thread's 8 MB stack.
static int element_depth(const maya::Element& e, int d = 0) {
    int best = d;
    std::visit([&](const auto& n) {
        using T = std::decay_t<decltype(n)>;
        if constexpr (std::is_same_v<T, maya::BoxElement>) {
            for (const auto& c : n.children)
                best = std::max(best, element_depth(c, d + 1));
        } else if constexpr (std::is_same_v<T, maya::ElementList>) {
            for (const auto& c : n.items)
                best = std::max(best, element_depth(c, d + 1));
        } else if constexpr (std::is_same_v<T, maya::ElementListRef>) {
            if (n.items_ref)
                for (const auto& c : *n.items_ref)
                    best = std::max(best, element_depth(c, d + 1));
        }
    }, e.inner);
    return best;
}

int main(int argc, char** argv) {
    if (argc < 2) {
        // No thread given: nothing to probe. This is how ctest invokes it,
        // so it must SUCCEED rather than report a failure — the probe is a
        // hand-run tool (see the header), not an unattended regression test.
        std::printf("real_thread_render_probe: no thread file given, nothing "
                    "to do.\nusage: %s <thread.json> [width]\n", argv[0]);
        return 0;
    }
    const std::filesystem::path p = argv[1];
    const int width = (argc > 2) ? std::atoi(argv[2]) : 120;

    auto t0 = std::chrono::steady_clock::now();
    std::printf("stage: load\n"); std::fflush(stdout);

    // A migrated thread is <id>.jsonl + .ofs + .meta.json; a legacy one is
    // a single <id>.json. Accept either, so the same probe measures the
    // same thread before and after migration.
    std::optional<Thread> loaded;
    if (p.extension() == ".jsonl") {
        auto log = ThreadLog::open_path(p);
        if (log) loaded = log->load_thread();
    } else {
        // Sub-time the load so the probe says WHERE the thread-switch
        // latency is: raw I/O, the JSON DOM parse, or the DOM->Thread walk.
        auto a = std::chrono::steady_clock::now();
        std::ifstream ifs(p, std::ios::binary);
        std::string buf;
        {
            std::error_code ec;
            auto sz = std::filesystem::file_size(p, ec);
            if (!ec && sz > 0) {
                buf.resize(static_cast<std::size_t>(sz));
                ifs.read(buf.data(), static_cast<std::streamsize>(sz));
                buf.resize(static_cast<std::size_t>(ifs.gcount()));
            }
        }
        auto b = std::chrono::steady_clock::now();
        auto io_ms = std::chrono::duration_cast<std::chrono::milliseconds>(b - a).count();
        nlohmann::json j = nlohmann::json::parse(buf, nullptr, false);
        auto c = std::chrono::steady_clock::now();
        auto dom_ms = std::chrono::duration_cast<std::chrono::milliseconds>(c - b).count();
        std::printf("  io: %lld ms (%zu bytes)  json-dom: %lld ms\n",
                    (long long)io_ms, buf.size(), (long long)dom_ms);
        std::fflush(stdout);
        if (auto r = persistence::load_thread_file(p)) loaded = std::move(*r);
    }
    std::printf("stage: load returned\n"); std::fflush(stdout);
    if (!loaded) {
        std::fprintf(stderr, "load failed: %s\n", p.c_str());
        return 2;
    }
    auto ms = [&](auto since) {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::steady_clock::now() - since).count();
    };
    std::printf("loaded %zu messages in %lld ms\n",
                loaded->messages.size(), (long long)ms(t0));
    std::fflush(stdout);

    Model m;
    m.d.current = std::move(*loaded);

    // MIGRATE=1: load then save straight back (what the first autosave after
    // a thread switch does) and report the file-size change. Legacy inline
    // base64 images become blob references, so an old image-heavy thread
    // shrinks once and loads fast forever after.
    if (std::getenv("MIGRATE")) {
        const auto before = std::filesystem::file_size(p);
        persistence::save_thread(m.d.current);
        persistence::flush_pending_saves();
        const auto after_path =
            persistence::threads_dir() / (m.d.current.id.value + ".json");
        std::error_code ec;
        const auto after = std::filesystem::file_size(after_path, ec);
        std::printf("migrate: %.1f MB -> %.1f MB (%s)\n",
                    before / 1e6, ec ? 0.0 : after / 1e6,
                    after_path.string().c_str());
        std::fflush(stdout);
        return 0;
    }

    // BISECT=1: binary-search the message prefix that first corrupts, so a
    // 1700-message thread names the ONE message that breaks the renderer
    // instead of leaving "somewhere in here".
    if (std::getenv("BISECT")) {
        const auto all = m.d.current.messages;
        std::printf("bisect over %zu messages\n", all.size());
        std::fflush(stdout);
        for (std::size_t n = 1; n <= all.size(); ++n) {
            Model probe;
            probe.d.current = m.d.current;
            probe.d.current.messages.assign(all.begin(), all.begin() + n);
            app::detail::rehydrate_frozen(probe);
            auto c = ui::conversation_config(probe);
            auto e = maya::Conversation{std::move(c)}.build();
            volatile std::size_t sz = maya::render_to_string(e, width).size();
            (void)sz;
            std::printf("ok through message %zu (role=%d, text=%zu bytes)\n",
                        n, (int)all[n - 1].role, all[n - 1].text.size());
            std::fflush(stdout);
        }
        std::printf("BISECT: no single prefix crashed\n");
        return 0;
    }

    auto t1 = std::chrono::steady_clock::now();
    std::printf("stage: rehydrate\n"); std::fflush(stdout);
    app::detail::rehydrate_frozen(m);
    std::printf("rehydrate_frozen: %lld ms, frozen=%zu through=%zu\n",
                (long long)ms(t1), m.ui.frozen.size(), m.ui.frozen_through);
    std::fflush(stdout);

    for (int w : {width, 80, 40}) {
        auto t2 = std::chrono::steady_clock::now();
        std::printf("stage: config w=%d\n", w); std::fflush(stdout);
        auto cfg = ui::conversation_config(m);
        std::printf("stage: build w=%d\n", w); std::fflush(stdout);
        auto el  = maya::Conversation{std::move(cfg)}.build();
        std::printf("stage: depth w=%d\n", w); std::fflush(stdout);
        std::printf("  element tree depth: %d\n", element_depth(el));
        std::fflush(stdout);
        // Deep-copy the tree BEFORE any rendering touches it. If this
        // throws/corrupts, the damage was done at BUILD time and the
        // renderer is an innocent bystander.
        if (std::getenv("COPY_PROBE")) {
            std::printf("stage: copy-probe (pre-render)\n"); std::fflush(stdout);
            maya::Element copy = el;
            std::printf("  copy ok, depth %d\n", element_depth(copy));
            std::fflush(stdout);
        }
        std::printf("stage: render w=%d\n", w); std::fflush(stdout);
        const std::string out = maya::render_to_string(el, w);
        std::printf("width %3d: built+rendered in %lld ms, %zu bytes\n",
                    w, (long long)ms(t2), out.size());
        std::fflush(stdout);
    }

    std::printf("OK\n");
    return 0;
}
