// read_grep_features_test — verifies the two Zed-derived enhancements:
//
//   1. read: a big file with NO recognisable code structure (a long log /
//      data dump) returns a "First 1 KiB" synthetic slice instead of a
//      full dump, with the anti-retry instruction. A big file that DOES
//      have declarations still returns the symbol OUTLINE.
//
//   2. grep: each emitted match block is headed by the enclosing
//      function/class breadcrumb when one is detectable from indentation.
//
// Drives the REAL registered ToolDefs (find("read") / find("grep")) over
// fixture files in a temp workspace.

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>

#include <nlohmann/json.hpp>

#include "agentty/tool/registry.hpp"
#include "agentty/tool/tools.hpp"
#include "agentty/tool/util/fs_helpers.hpp"

namespace fs = std::filesystem;
using json = nlohmann::json;
using namespace agentty;

namespace {

int g_fails = 0;
void check(bool ok, const char* what) {
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", what); ++g_fails; }
    else     { std::fprintf(stderr, "ok:   %s\n", what); }
}

void write_file(const fs::path& p, const std::string& body) {
    std::ofstream o(p, std::ios::binary);
    o.write(body.data(), static_cast<std::streamsize>(body.size()));
}

std::string run(const char* tool, const json& args) {
    const auto* def = tools::find(tool);
    if (!def) { std::fprintf(stderr, "FAIL: tool %s not registered\n", tool); ++g_fails; return {}; }
    auto r = def->execute(args);
    if (!r.has_value()) {
        std::fprintf(stderr, "  (tool %s errored: %s)\n", tool, r.error().detail.c_str());
        return {};
    }
    return r->text;
}

bool contains(const std::string& hay, const std::string& needle) {
    return hay.find(needle) != std::string::npos;
}

} // namespace

int main() {
    fs::path dir = fs::temp_directory_path()
                 / ("agentty_rgf_" + std::to_string(std::random_device{}()));
    fs::create_directories(dir);
    tools::util::set_workspace_root(dir);

    // ── Fixture 1: a big structureless data file (no declarations) ──────
    {
        std::string body;
        // ~40 KiB of "key=value" log lines — no fn/class/struct anywhere.
        for (int i = 0; i < 1200; ++i)
            body += "2026-01-01T00:00:" + std::to_string(i % 60)
                  + " level=info request_id=" + std::to_string(i)
                  + " latency_ms=" + std::to_string((i * 7) % 999) + "\n";
        check(body.size() > 32 * 1024, "fixture1 exceeds 32 KiB outline threshold");
        write_file(dir / "app.log", body);

        std::string out = run("read", json{{"path", (dir / "app.log").string()}});
        check(contains(out, "First 1 KiB"),
              "structureless big file -> First 1 KiB fallback");
        check(contains(out, "Do NOT retry this read without a line range"),
              "1KB fallback carries anti-retry instruction");
        check(contains(out, "lines total"),
              "1KB fallback reports total line count");
        // The peek must be bounded well under the full body.
        check(out.size() < 4096, "1KB fallback body is small (<4KB)");
        // An explicit range must bypass the fallback and return real lines.
        std::string ranged = run("read",
            json{{"path", (dir / "app.log").string()}, {"start_line", 1}, {"end_line", 3}});
        check(contains(ranged, "level=info request_id=0"),
              "explicit range bypasses fallback, returns real content");
        check(!contains(ranged, "First 1 KiB"),
              "explicit range never shows the fallback header");
    }

    // ── Fixture 2: a big file WITH declarations -> outline, not 1KB ─────
    {
        std::string body;
        for (int i = 0; i < 1200; ++i) {
            body += "int function_" + std::to_string(i) + "(int x) {\n";
            body += "    return x + " + std::to_string(i) + ";\n";
            body += "}\n\n";
        }
        check(body.size() > 32 * 1024, "fixture2 exceeds outline threshold");
        write_file(dir / "many.cpp", body);

        std::string out = run("read", json{{"path", (dir / "many.cpp").string()}});
        check(contains(out, "File outline retrieved"),
              "big code file -> symbol OUTLINE (not 1KB fallback)");
        check(!contains(out, "First 1 KiB"),
              "code file does not take the 1KB path");
        check(contains(out, "function_0"), "outline lists a symbol");
    }

    // ── Fixture 3: grep enclosing-symbol breadcrumb ─────────────────────
    {
        std::string body =
            "namespace demo {\n"
            "\n"
            "void compute_widget_total(int n) {\n"
            "    int accumulator = 0;\n"
            "    for (int i = 0; i < n; ++i) {\n"
            "        accumulator += MAGIC_FACTOR;\n"   // <- the match line
            "    }\n"
            "}\n"
            "\n"
            "} // namespace demo\n";
        write_file(dir / "widget.cpp", body);

        std::string out = run("grep",
            json{{"pattern", "MAGIC_FACTOR"}, {"glob", "*.cpp"}, {"path", dir.string()}});
        check(contains(out, "MAGIC_FACTOR"), "grep finds the match");
        check(contains(out, "compute_widget_total"),
              "grep block header carries enclosing-function breadcrumb");
        // The breadcrumb separator (U+203A ›) precedes the L-range.
        check(contains(out, "\xe2\x80\xba L"),
              "breadcrumb uses the > separator before the line range");
    }

    // ── Fixture 4: Python — climb past for/if to the enclosing def ──────
    {
        std::string body =
            "class Widget:\n"
            "    def render(self, ctx):\n"
            "        for item in self.items:\n"
            "            if item.visible:\n"
            "                ctx.draw(item.SPECIAL_TOKEN)\n";
        write_file(dir / "sample.py", body);
        std::string out = run("grep",
            json{{"pattern", "SPECIAL_TOKEN"}, {"glob", "*.py"}, {"path", dir.string()}});
        check(contains(out, "def render"),
              "python breadcrumb climbs past for/if to the def");
    }

    // ── Fixture 5: Rust — climb past a closure body to fn ──────────────
    {
        std::string body =
            "impl Renderer {\n"
            "    fn paint(&self, frame: &mut Frame) {\n"
            "        let total = self.cells.iter().map(|c| {\n"
            "            c.weight * SCALE_FACTOR\n"
            "        }).sum();\n"
            "    }\n"
            "}\n";
        write_file(dir / "sample.rs", body);
        std::string out = run("grep",
            json{{"pattern", "SCALE_FACTOR"}, {"glob", "*.rs"}, {"path", dir.string()}});
        check(contains(out, "fn paint"),
              "rust breadcrumb climbs past the closure body to fn");
    }

    fs::remove_all(dir);

    if (g_fails == 0) std::fprintf(stderr, "\nALL PASSED\n");
    else              std::fprintf(stderr, "\n%d FAILURE(S)\n", g_fails);
    return g_fails == 0 ? 0 : 1;
}
