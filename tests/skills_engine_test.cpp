// skills_engine_test — correctness for the Agent Skills engine
// (agentskills.io implementation): discovery across native + interop
// roots, project-shadows-user precedence, lenient frontmatter parsing
// (unquoted colons, name/dir mismatch, missing description), optional
// fields (compatibility / allowed-tools / disable-model-invocation),
// tier-3 resource enumeration, activation payload shape, activation
// dedup + reset, catalog filtering, and the read-allowlist gate that
// lets `read` fetch bundled resources outside the workspace while the
// write gate stays strict.
//
// Strategy: build a sandbox HOME + cwd under a temp dir, point the
// process at them (setenv HOME + chdir), then drive the real engine.
// The engine's cache is keyed on root/file mtimes, and every test
// stage writes new files, so cross-stage contamination is impossible
// as long as stages use distinct skill names.

#include "agtest.hpp"

#include "agentty/tool/skills.hpp"
#include "agentty/tool/util/fs_helpers.hpp"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;
using namespace agentty::tools;



static void write_file_at(const fs::path& p, const std::string& body) {
    fs::create_directories(p.parent_path());
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f << body;
}

// Touch the roots so the engine's mtime signature changes even when the
// filesystem's mtime granularity is coarse: writing a brand-new SKILL.md
// adds its own mtime to the signature, which is sufficient.

TEST_CASE("skills engine") {
    agtest::ScopedEnvSandbox _env_guard;
    std::error_code ec;
    fs::path base = fs::temp_directory_path(ec) / "agentty_skills_test";
    fs::remove_all(base, ec);
    fs::path home = base / "home";
    fs::path work = base / "work";
    fs::create_directories(home);
    fs::create_directories(work);

#if defined(_WIN32)
    _putenv_s("HOME", home.string().c_str());
    // The user-scope base is util::user_root() ($AGENTTY_HOME, falling
    // back to $HOME/.agentty). Point it at THIS test's home so the
    // skills the test writes under home/.agentty are the ones discovery
    // finds — otherwise it resolves the shared per-binary sandbox.
    _putenv_s("AGENTTY_HOME", "");   // fall back to $HOME/.agentty
#else
    setenv("HOME", home.string().c_str(), 1);
    unsetenv("AGENTTY_HOME");   // fall back to $HOME/.agentty (this test's home)
#endif
    fs::current_path(work);
    util::set_workspace_root(work);

    // ── Stage 1: discovery across roots + precedence ─────────────────
    // user native
    write_file_at(home / ".agentty/skills/alpha/SKILL.md",
        "---\nname: alpha\ndescription: user native alpha\n---\nUSER BODY\n");
    // user interop (.agents) — same name, must be shadowed by native
    write_file_at(home / ".agents/skills/alpha/SKILL.md",
        "---\nname: alpha\ndescription: interop alpha\n---\nINTEROP BODY\n");
    // user .claude compat — unique name, must be discovered
    write_file_at(home / ".claude/skills/claude-only/SKILL.md",
        "---\nname: claude-only\ndescription: from claude dir\n---\nCC BODY\n");
    // project native — same name as user alpha, must WIN
    write_file_at(work / ".agentty/skills/alpha/SKILL.md",
        "---\nname: alpha\ndescription: project alpha\n---\nPROJECT BODY\n");
    // project interop
    write_file_at(work / ".agents/skills/beta/SKILL.md",
        "---\nname: beta\ndescription: project interop beta\n---\nBETA BODY\n");

    {
        const auto* a = skills::find("alpha");
        CHECK(a != nullptr);
        if (a) {
            CHECK(a->source == "project");          // project shadows user
            CHECK(a->body == "PROJECT BODY");
            CHECK(!a->dir.empty());
        }
        CHECK(skills::find("claude-only") != nullptr);   // .claude compat
        CHECK(skills::find("beta") != nullptr);          // .agents interop
    }

    // ── Stage 2: lenient parsing ─────────────────────────────────────
    // Unquoted colon in description (invalid YAML other parsers choke on).
    write_file_at(work / ".agentty/skills/colons/SKILL.md",
        "---\nname: colons\ndescription: Use when: things have colons\n---\nB\n");
    // name/dir mismatch — loads anyway, frontmatter name wins.
    write_file_at(work / ".agentty/skills/dirname-x/SKILL.md",
        "---\nname: othername\ndescription: mismatch ok\n---\nB\n");
    // No frontmatter at all — body-first-line becomes the description.
    write_file_at(work / ".agentty/skills/bare/SKILL.md",
        "Just a bare instruction doc.\nMore text.\n");
    {
        const auto* c = skills::find("colons");
        CHECK(c && c->description == "Use when: things have colons");
        CHECK(skills::find("othername") != nullptr);
        CHECK(skills::find("dirname-x") == nullptr);  // frontmatter name won
        const auto* b = skills::find("bare");
        CHECK(b && b->description == "Just a bare instruction doc.");
    }

    // ── Stage 3: optional fields + catalog filtering ─────────────────
    write_file_at(work / ".agentty/skills/full-meta/SKILL.md",
        "---\n"
        "name: full-meta\n"
        "description: has every optional field\n"
        "compatibility: Requires python3\n"
        "allowed-tools: bash read\n"
        "license: Apache-2.0\n"
        "metadata:\n"
        "  author: example-org\n"
        "  version: \"1.0\"\n"
        "---\nMETA BODY\n");
    write_file_at(work / ".agentty/skills/hidden/SKILL.md",
        "---\nname: hidden\ndescription: user-explicit only\n"
        "disable-model-invocation: true\n---\nHIDDEN BODY\n");
    // Block-scalar description (folded `>-`), the Claude Code-ism.
    write_file_at(work / ".agentty/skills/folded/SKILL.md",
        "---\n"
        "name: folded\n"
        "description: >-\n"
        "  First folded line\n"
        "  second folded line\n"
        "---\nFOLD BODY\n");
    {
        const auto* f = skills::find("full-meta");
        CHECK(f && f->compatibility == "Requires python3");
        CHECK(f && f->allowed_tools == "bash read");
        CHECK(f && f->license == "Apache-2.0");
        CHECK(f && f->metadata.size() == 2);
        if (f && f->metadata.size() == 2) {
            CHECK(f->metadata[0].first == "author"
                  && f->metadata[0].second == "example-org");
            CHECK(f->metadata[1].first == "version"
                  && f->metadata[1].second == "1.0");
        }
        const auto* fo = skills::find("folded");
        CHECK(fo && fo->description ==
              "First folded line second folded line");
        CHECK(fo && fo->body == "FOLD BODY");
        // hidden: findable explicitly, absent from the catalog.
        const auto* h = skills::find("hidden");
        CHECK(h && h->user_only);
        auto cat = skills::catalog_block();
        CHECK(cat.find("full-meta") != std::string::npos);
        CHECK(cat.find("hidden") == std::string::npos);
        // Catalog mentions the tier-3 contract.
        CHECK(cat.find("skill") != std::string::npos);
    }

    // ── Stage 4: tier-3 resources + activation payload ───────────────
    write_file_at(work / ".agentty/skills/with-res/SKILL.md",
        "---\nname: with-res\ndescription: bundles resources\n---\n"
        "Run scripts/go.sh then read references/REF.md\n");
    write_file_at(work / ".agentty/skills/with-res/scripts/go.sh",
        "#!/bin/sh\necho hi\n");
    write_file_at(work / ".agentty/skills/with-res/references/REF.md",
        "deep reference\n");
    {
        const auto* w = skills::find("with-res");
        CHECK(w != nullptr);
        if (w) {
            CHECK(w->resources.size() == 2);
            // Sorted, relative, forward slashes; SKILL.md excluded.
            CHECK(w->resources[0] == "references/REF.md");
            CHECK(w->resources[1] == "scripts/go.sh");
            auto pay = skills::activation_payload(*w);
            CHECK(pay.find("<skill_content name=\"with-res\">") == 0);
            CHECK(pay.find("Skill directory: ") != std::string::npos);
            CHECK(pay.find("<skill_resources>") != std::string::npos);
            CHECK(pay.find("scripts/go.sh") != std::string::npos);
            CHECK(pay.find("</skill_content>") != std::string::npos);
        }
    }

    // ── Stage 5: activation dedup + reset ────────────────────────
    {
        skills::reset_activations();
        CHECK(skills::note_activated("alpha") == true);    // first load
        CHECK(skills::note_activated("alpha") == false);   // dedup
        CHECK(skills::note_activated("beta") == true);     // independent
        skills::reset_activations();                       // thread swap
        CHECK(skills::note_activated("alpha") == true);    // loadable again
    }

    // ── Stage 5b: spec lint ───────────────────────────────────
    {
        // Clean skill → no diagnostics.
        const auto* f = skills::find("full-meta");
        CHECK(f && skills::lint(*f).empty());
        // Violations → diagnostics fire (loading stayed lenient).
        skills::Skill bad;
        bad.name = "Bad--Name-";
        bad.description = "";
        auto diags = skills::lint(bad);
        CHECK(diags.size() >= 3);   // charset + double hyphen + edge + desc
        // name/dir mismatch caught.
        const auto* mm = skills::find("othername");
        bool has_mismatch = false;
        if (mm) for (const auto& d : skills::lint(*mm))
            if (d.find("does not match parent directory") != std::string::npos)
                has_mismatch = true;
        CHECK(has_mismatch);
    }

    // ── Stage 6.5: nested (grouped) skills ──────────────────────
    // Skills may live at ANY depth below a discovery root; the name is
    // the path below the root with segments joined by '-'.
    write_file_at(work / ".agentty/skills/embedded/startup/SKILL.md",
        "---\nname: embedded-startup\ndescription: nested two levels\n---\n"
        "NESTED BODY\n");
    write_file_at(work / ".agentty/skills/perf/alloc/SKILL.md",
        "---\ndescription: name falls back to the joined path slug\n---\n"
        "IMPLICIT NAME BODY\n");
    write_file_at(work / ".agentty/skills/deep/a/b/c/SKILL.md",
        "---\nname: deep-a-b-c\ndescription: four levels down\n---\nBODY\n");
    // Hidden directories are storage, never skill territory.
    write_file_at(work / ".agentty/skills/.cache/junk/SKILL.md",
        "---\nname: cache-junk\ndescription: must not be discovered\n---\nBODY\n");
    // Joined-name collision: the shallower skill wins.
    write_file_at(work / ".agentty/skills/pair/SKILL.md",
        "---\nname: pair\ndescription: shallow\n---\nSHALLOW\n");
    write_file_at(work / ".agentty/skills/x/pair/SKILL.md",
        "---\nname: pair\ndescription: deeper\n---\nDEEPER\n");
    {
        const auto* n = skills::find("embedded-startup");
        CHECK(n && n->body == "NESTED BODY");
        CHECK(n && n->source == "project");
        // Lint compares against the LEAF directory, not the joined name.
        if (n) {
            bool mismatch = false;
            for (const auto& d : skills::lint(*n))
                if (d.find("does not match parent directory") != std::string::npos)
                    mismatch = true;
            CHECK(!mismatch);
        }
        // Explicit `name:` missing → fallback is the joined path slug.
        const auto* impl = skills::find("perf-alloc");
        CHECK(impl && impl->body == "IMPLICIT NAME BODY");
        const auto* deep = skills::find("deep-a-b-c");
        CHECK(deep && deep->body == "BODY");
        // Hidden dirs never yield skills.
        CHECK(skills::find("cache-junk") == nullptr);
        // Shallower beats deeper on a joined-name collision.
        const auto* p = skills::find("pair");
        CHECK(p && p->description == "shallow");
        CHECK(p && p->body == "SHALLOW");
    }

    // A nested skill is NOT a resource of the skill it sits inside.
    write_file_at(work / ".agentty/skills/host/SKILL.md",
        "---\nname: host\ndescription: holds a nested skill\n---\nHOST BODY\n");
    write_file_at(work / ".agentty/skills/host/nested/inner/SKILL.md",
        "---\nname: host-nested-inner\ndescription: inside host\n---\nINNER\n");
    write_file_at(work / ".agentty/skills/host/refs/NOTE.md", "plain ref\n");
    {
        const auto* h = skills::find("host");
        CHECK(h && h->resources.size() == 1);
        if (h && h->resources.size() == 1)
            CHECK(h->resources[0] == "refs/NOTE.md");
        CHECK(skills::find("host-nested-inner") != nullptr);
    }

    // Project shadows user with the same nested name.
    write_file_at(home / ".agentty/skills/embedded/startup/SKILL.md",
        "---\nname: embedded-startup\ndescription: user variant\n---\nUSER BODY\n");
    {
        const auto* n = skills::find("embedded-startup");
        CHECK(n && n->source == "project" && n->body == "NESTED BODY");
    }

    // ── Stage 6: read-allowlist gate ─────────────────────────────────
    // A USER-scope skill's resources live outside the workspace; the
    // read gate must pass them, the write gate must still refuse.
    write_file_at(home / ".agentty/skills/usr-res/SKILL.md",
        "---\nname: usr-res\ndescription: user skill with resource\n---\nB\n");
    write_file_at(home / ".agentty/skills/usr-res/references/SECRET-FREE.md",
        "outside-workspace resource\n");
    {
        const auto* u = skills::find("usr-res");   // discovery registers allowlist
        CHECK(u != nullptr);
        auto res = (home / ".agentty/skills/usr-res/references/SECRET-FREE.md").string();
        CHECK(util::is_read_allowlisted(res));
        auto ok = util::make_readable_path_checked(res, "read");
        CHECK(ok.has_value());
        // Write gate is untouched by the allowlist.
        auto wr = util::make_workspace_path_checked(res, "write");
        CHECK(!wr.has_value());
        // And a non-skill out-of-workspace path still fails the read gate.
        auto bad = util::make_readable_path_checked(
            (home / "unrelated.txt").string(), "read");
        CHECK(!bad.has_value());
    }

    fs::current_path(base, ec);   // leave `work` so cleanup can remove it
    fs::remove_all(base, ec);

}
