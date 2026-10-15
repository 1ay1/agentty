// settings_input_bench — keystroke-to-paint latency on every settings pane.
//
// Each key goes the real way: on_key (key -> Msg) -> update -> view -> paint.
// Prints the worst and median per pane so a slow keystroke has a name. Not a
// test: microsecond assertions would test the machine, not the code.

#include "agentty/runtime/app/deps.hpp"
#include "agentty/runtime/app/subscribe.hpp"
#include "agentty/runtime/app/update.hpp"
#include "agentty/runtime/model.hpp"
#include "agentty/runtime/msg.hpp"
#include "agentty/runtime/panel/settings/categories.hpp"
#include "agentty/runtime/view/view.hpp"

#include <maya/render/canvas.hpp>
#include <maya/render/renderer.hpp>
#include <maya/style/theme.hpp>
#include <maya/terminal/input.hpp>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <string>
#include <vector>

using namespace agentty;
using clk = std::chrono::steady_clock;

namespace {

store::Settings g_settings;

void install_stub_deps() {
    app::install_deps(app::Deps{
        .stream         = [](const maya::unlocked&, auto, auto) {},
        .save_thread    = [](const Thread&) {},
        .delete_thread  = [](const auto&) {},
        .load_threads   = [] { return std::vector<Thread>{}; },
        .load_thread    = [](const ThreadId&) { return std::optional<Thread>{}; },
        .load_settings  = [] { return g_settings; },
        .save_settings  = [](const store::Settings& s) { g_settings = s; },
        .new_thread_id  = [] { return ThreadId{"t-bench"}; },
        .title_from     = [](std::string_view t) { return std::string{t}; },
        .auth           = {},
    });
}

maya::KeyEvent sk(maya::SpecialKey k) { return maya::KeyEvent{k, {}, {}}; }
maya::KeyEvent ch(char32_t c) { return maya::KeyEvent{maya::CharKey{c}, {}, {}}; }

struct Result { double worst_us = 0, median_us = 0; int keys = 0; };

double med(std::vector<double> v) {
    std::sort(v.begin(), v.end());
    return v[v.size() / 2];
}
double mx(const std::vector<double>& v) { return *std::max_element(v.begin(), v.end()); }

// A key mix a user actually types: move, adjust, enter, type, leave.
std::vector<maya::KeyEvent> script() {
    using S = maya::SpecialKey;
    std::vector<maya::KeyEvent> k;
    for (int round = 0; round < 6; ++round) {
        k.push_back(sk(S::Down));
        k.push_back(sk(S::Right));
        k.push_back(sk(S::Left));
        k.push_back(sk(S::Enter));
        for (char32_t c : std::u32string{U"42ab"}) k.push_back(ch(c));
        k.push_back(sk(S::Backspace));
        k.push_back(sk(S::Enter));
        k.push_back(sk(S::Down));
    }
    return k;
}

Result run(Model m, Msg open, const char* name) {
    m = app::update(std::move(m), std::move(open)).first;
    maya::StylePool pool;
    maya::Canvas canvas(120, 50, &pool);
    std::vector<double> us, up, vw, pt;
    for (const auto& ev : script()) {
        const auto t0 = clk::now();
        if (auto msg = app::on_key(m, ev))
            m = app::update(std::move(m), std::move(*msg)).first;
        const auto t1 = clk::now();
        auto el = ui::view(m);
        const auto t2 = clk::now();
        maya::render_tree(el, canvas, pool, maya::theme::native, true);
        const auto t3 = clk::now();
        const auto d = [](auto a, auto b) {
            return std::chrono::duration<double, std::micro>(b - a).count();
        };
        us.push_back(d(t0, t3)); up.push_back(d(t0, t1));
        vw.push_back(d(t1, t2)); pt.push_back(d(t2, t3));
    }
    Result r{mx(us), med(us), static_cast<int>(us.size())};
    std::printf("  %-11s total %6.0f/%6.0f  update %6.0f/%6.0f  view %6.0f/%6.0f  paint %6.0f/%6.0f\n",
                name, med(us), mx(us), med(up), mx(up), med(vw), mx(vw), med(pt), mx(pt));
    return r;
}

}  // namespace

int main() {
    install_stub_deps();
    std::printf("median/worst microseconds per key, 120x50\n");
    run(Model{}, Msg{OpenPalette{}}, "(palette)");
    const auto L = [](settings::Category c) { return Msg{OpenSettingsList{c}}; };
    run(Model{}, L(settings::Category::General),  "Settings");
    run(Model{}, L(settings::Category::Plugins),  "Plugins");
    run(Model{}, L(settings::Category::Commands), "Commands");
    run(Model{}, L(settings::Category::Agents),   "Subagents");
    run(Model{}, L(settings::Category::Hooks),    "Hooks");
    run(Model{}, Msg{OpenAppearance{}}, "Appearance");
    run(Model{}, Msg{OpenSandbox{}},    "Sandbox");
    run(Model{}, Msg{OpenSmartMode{}},  "Smart Mode");
    run(Model{}, Msg{OpenRag{}},        "RAG");
    run(Model{}, Msg{OpenWebSearch{}},  "Web Search");
    return 0;
}
