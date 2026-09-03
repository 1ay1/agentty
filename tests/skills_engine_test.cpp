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

    // ── Depth precedence is by DEPTH, not by byte order ──────────────
    //
    // `pair` vs `pair/x` above happens to sort shallow-first, so it does
    // not exercise the rule. This pair does: '-' (0x2D) sorts BEFORE '/'
    // (0x2F), so plain lexicographic order visits `ambig/dup/SKILL.md`
    // first and the DEEPER skill would win. Both slug to `ambig-dup`, so
    // an existing flat skill would be silently displaced the moment an
    // unrelated group folder appeared beside it — the exact regression
    // "flat layouts keep their names unchanged" rules out.
    write_file_at(work / ".agentty/skills/ambig-dup/SKILL.md",
        "---\ndescription: flat and shallow\n---\nFLAT\n");
    write_file_at(work / ".agentty/skills/ambig/dup/SKILL.md",
        "---\ndescription: nested and deeper\n---\nNESTED\n");
    {
        const auto* a = skills::find("ambig-dup");
        CHECK(a != nullptr);
        CHECK(a && a->body == "FLAT");
        CHECK(a && a->description == "flat and shallow");
    }

    // ── Derived names stay inside the spec charset ───────────────────
    //
    // A name is what the user types as `/name`, and the slash-command
    // parser splits its token on whitespace — so a name containing a
    // space is discoverable, listed, and permanently un-invokable.
    // Replacing only '/' let spaces, underscores and uppercase through.
    write_file_at(work / ".agentty/skills/My Group/Sub_Dir/SKILL.md",
        "---\ndescription: charset\n---\nCHARSET\n");
    {
        const auto* c = skills::find("my-group-sub-dir");
        CHECK(c != nullptr);
        CHECK(c && c->body == "CHARSET");
        // The raw, unsanitized form must not exist under any spelling.
        CHECK(skills::find("My Group-Sub_Dir") == nullptr);
        for (const auto& sk : skills::all()) {
            bool clean = true;
            for (unsigned char ch : sk.name) {
                const bool ok = (ch >= 'a' && ch <= 'z')
                             || (ch >= '0' && ch <= '9') || ch == '-';
                if (!ok) { clean = false; break; }
            }
            // Frontmatter `name:` may legitimately override the slug, so
            // only DERIVED names (slug == name) are charset-bound.
            if (sk.slug == sk.name)
                CHECK_MESSAGE(clean, "derived name outside [a-z0-9-]: " << sk.name);
        }
    }

    // Runs collapse and edges trim: `a  b` / `-lead-` never yield
    // doubled or edge hyphens.
    write_file_at(work / ".agentty/skills/-Odd  Name-/SKILL.md",
        "---\ndescription: collapse\n---\nCOLLAPSE\n");
    {
        const auto* c = skills::find("odd-name");
        CHECK(c && c->body == "COLLAPSE");
    }

    // ── A symlinked skill is named where it was PUT ──────────────────
    //
    // Symlinking a skill dir into the library is a supported layout (the
    // flat scan discovered it, and the walk probes symlinks directly
    // because the iterator will not follow them). Its name must come from
    // the link's name in the library, NOT from wherever the bytes live:
    // fs::relative canonicalizes, so measuring the slug that way escaped
    // the root as `../../elsewhere/real` and named the skill after an
    // absolute filesystem path.
    {
        const fs::path target = base / "external" / "real-skill-dir";
        write_file_at(target / "SKILL.md",
            "---\ndescription: lives outside the library\n---\nLINKED\n");
        std::error_code lec;
        fs::create_directory_symlink(
            target, work / ".agentty/skills/aliased", lec);
        if (!lec) {   // skip where symlinks are unavailable (some CI hosts)
            const auto* l = skills::find("aliased");
            CHECK(l != nullptr);
            CHECK(l && l->body == "LINKED");
            // The resolved path must never leak into the name.
            CHECK(skills::find("real-skill-dir") == nullptr);
            for (const auto& sk : skills::all())
                CHECK(sk.name.find("..") == std::string::npos);
        }
    }

    // ── The walk is bounded ──────────────────────────────────────────
    //
    // Depth stops at kMaxSkillDepth: group folders organize a library,
    // they do not nest arbitrarily. An unbounded walk both traversed a
    // stray large tree in full on every cache miss and produced names
    // too long to type (a 40-deep nest yielded 238 characters).
    {
        fs::path deep = work / ".agentty/skills/toodeep";
        for (int i = 0; i < 8; ++i) deep /= ("lvl" + std::to_string(i));
        write_file_at(deep / "SKILL.md",
            "---\ndescription: past the cap\n---\nTOODEEP\n");
        int past_cap = 0;
        for (const auto& sk : skills::all())
            if (sk.description == "past the cap") ++past_cap;
        CHECK(past_cap == 0);
        // …and every name that IS discovered is typeable.
        for (const auto& sk : skills::all())
            CHECK(sk.name.size() <= skills::kMaxSlugLen);
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

TEST_CASE("skills catalog cap: AGENTTY_MAX_SKILLS override") {
    agtest::ScopedEnvSandbox _env_guard;
    std::error_code ec;
    fs::path base = fs::temp_directory_path(ec) / "agentty_skills_cap_test";
    fs::remove_all(base, ec);
    fs::path home = base / "home";
    fs::path work = base / "work";
    fs::create_directories(home);
    fs::create_directories(work);

#if defined(_WIN32)
    _putenv_s("HOME", home.string().c_str());
    _putenv_s("AGENTTY_HOME", "");
#else
    setenv("HOME", home.string().c_str(), 1);
    unsetenv("AGENTTY_HOME");
#endif
    fs::current_path(work);
    util::set_workspace_root(work);

    // The knob is read per discovery pass, and all() rescans only when its
    // mtime signature changes — so each sub-case below shifts the sig by
    // changing how many SKILL.md mtimes the (cap-bounded) walk collects.
    // Adjacent sub-cases never share a signature while expecting different
    // results, so no forced cache-buster writes are needed.

    // RAII on the knob itself: restore (or clear) even if a CHECK throws.
    const char* old_raw = std::getenv("AGENTTY_MAX_SKILLS");
    const bool  had_old = old_raw != nullptr;
    const std::string old_val = had_old ? old_raw : "";
    struct Restore {
        const bool had; const std::string val;
        ~Restore() {
#if defined(_WIN32)
            if (had) _putenv_s("AGENTTY_MAX_SKILLS", val.c_str());
            else     _putenv_s("AGENTTY_MAX_SKILLS", "");
#else
            if (had) setenv("AGENTTY_MAX_SKILLS", val.c_str(), 1);
            else     unsetenv("AGENTTY_MAX_SKILLS");
#endif
        }
    } _restore{had_old, old_val};

    auto set_env = [](const char* v) {
#if defined(_WIN32)
        _putenv_s("AGENTTY_MAX_SKILLS", v);   // empty value removes (CRT)
#else
        setenv("AGENTTY_MAX_SKILLS", v, 1);
#endif
    };
    auto unset_env = []() {
#if defined(_WIN32)
        _putenv_s("AGENTTY_MAX_SKILLS", "");
#else
        unsetenv("AGENTTY_MAX_SKILLS");
#endif
    };

    // 80 skills — over the default cap in every scenario below.
    constexpr std::size_t kSkills = 80;
    for (std::size_t i = 0; i < kSkills; ++i) {
        const std::string slug = "cap-skill-" + std::to_string(i);
        write_file_at(home / ".agentty/skills" / slug / "SKILL.md",
            "---\nname: " + slug + "\ndescription: cap test\n---\nB\n");
    }

    // ── Default (unset): kMaxSkills entries survive the walk AND the
    // catalog slice — the cap applies to the WORK as well as the RESULT.
    unset_env();
    CHECK(skills::all().size() == skills::kMaxSkills);

    // ── Raised: every discovered skill is catalogued, including the ones
    // the default-capped walk never even visited.
    set_env("200");
    CHECK(skills::all().size() == kSkills);
    CHECK(skills::find("cap-skill-79") != nullptr);

    // ── Lowered: the catalog truncates to the override.
    set_env("10");
    CHECK(skills::all().size() == 10);

    // ── Clamp floor: below the floor pins to it.
    set_env("2");
    CHECK(skills::all().size() == 8);

    // ── Malformed: garbage keeps the default cap.
    set_env("not-a-number");
    CHECK(skills::all().size() == skills::kMaxSkills);

    // ── ONE resolution per discovery pass ───────────────────────────
    // The cap bounds scan_root's WALK, so it used to be consulted per
    // directory entry — and on the malformed path that emitted the
    // dbglog breadcrumb per entry: measured 120 ERROR-level lines for a
    // single pass over 40 skills. all() runs every turn and dbglog feeds
    // the crash flight recorder, so one typo'd env var displaced the
    // diagnostics a crash dump exists to preserve.
    //
    // Assert the property at its source rather than counting log lines
    // (logx's file sink latches on first use, so sink-based counting is
    // unreliable inside the shared test binary): a discovery pass must
    // resolve the cap EXACTLY once, no matter how many entries it walks
    // or how many roots it visits.
    {
        // Force a rescan — the cache is keyed on mtimes, so touch a file.
        write_file_at(home / ".agentty/skills/cap-skill-0/SKILL.md",
            "---\nname: cap-skill-0\ndescription: touched\n---\nB\n");
        set_env("not-a-number");          // the path that used to spam
        const auto before = skills::debug_cap_resolutions();
        (void)skills::all();
        const auto after = skills::debug_cap_resolutions();
        CHECK_MESSAGE(after - before == 1,
            "a discovery pass resolved the cap " << (after - before)
            << " times; it must resolve exactly once (a malformed value "
            "logs an ERROR-level breadcrumb per resolution, into the "
            "crash flight recorder)");
    }

    fs::current_path(base, ec);
    fs::remove_all(base, ec);
}
