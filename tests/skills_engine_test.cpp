// skills_engine_test — correctness for the Agent Skills engine
// (agentskills.io implementation): discovery across native + interop
// roots, project-shadows-user precedence, lenient frontmatter parsing
// (unquoted colons, name/dir mismatch, missing description), optional
// fields (compatibility / allowed-tools / disable-model-invocation),
// tier-3 resource enumeration, activation payload shape, activation
// derived from the visible transcript (incl. the post-/compact case the old
// stored set got wrong), catalog filtering, and the read-allowlist gate that
// lets `read` fetch bundled resources outside the workspace while the
// write gate stays strict.
//
// Strategy: build a sandbox HOME + cwd under a temp dir, point the
// process at them (setenv HOME + chdir), then drive the real engine.
// The engine's cache is keyed on root/file mtimes, and every test
// stage writes new files, so cross-stage contamination is impossible
// as long as stages use distinct skill names.

#include "agtest.hpp"

#include "agentty/domain/conversation.hpp"   // Thread, visible_text
#include "agentty/tool/skills.hpp"
#include "agentty/tool/util/fs_helpers.hpp"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;
using namespace agentty::tools;
using agentty::Message;
using agentty::Role;
using agentty::Thread;
using agentty::ToolCallId;
using agentty::ToolName;
using agentty::ToolUse;
using agentty::visible_text;



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
    util::set_workspace_root(::agentty::IoAccess::grant(), work);

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
        const auto a = skills::find(::agentty::IoAccess::grant(), "alpha");
        CHECK(a.has_value());
        if (a) {
            CHECK(a->source == "project");          // project shadows user
            CHECK(a->body == "PROJECT BODY");
            CHECK(!a->dir.empty());
        }
        CHECK(skills::find(::agentty::IoAccess::grant(), "claude-only").has_value());   // .claude compat
        CHECK(skills::find(::agentty::IoAccess::grant(), "beta").has_value());          // .agents interop
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
        const auto c = skills::find(::agentty::IoAccess::grant(), "colons");
        CHECK(c && c->description == "Use when: things have colons");
        CHECK(skills::find(::agentty::IoAccess::grant(), "othername").has_value());
        CHECK(!skills::find(::agentty::IoAccess::grant(), "dirname-x").has_value());  // frontmatter name won
        const auto b = skills::find(::agentty::IoAccess::grant(), "bare");
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
        const auto f = skills::find(::agentty::IoAccess::grant(), "full-meta");
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
        const auto fo = skills::find(::agentty::IoAccess::grant(), "folded");
        CHECK(fo && fo->description ==
              "First folded line second folded line");
        CHECK(fo && fo->body == "FOLD BODY");
        // hidden: findable explicitly, absent from the catalog.
        const auto h = skills::find(::agentty::IoAccess::grant(), "hidden");
        CHECK(h && h->user_only);
        auto cat = skills::catalog_block(::agentty::IoAccess::grant());
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
        const auto w = skills::find(::agentty::IoAccess::grant(), "with-res");
        CHECK(w.has_value());
        if (w) {
            CHECK(w->resources.size() == 2);
            // Sorted, relative, forward slashes; SKILL.md excluded.
            CHECK(w->resources[0] == "references/REF.md");
            CHECK(w->resources[1] == "scripts/go.sh");
            auto pay = skills::activation_payload(::agentty::IoAccess::grant(), *w);
            CHECK(pay.find("<skill_content name=\"with-res\">") == 0);
            CHECK(pay.find("Skill directory: ") != std::string::npos);
            CHECK(pay.find("<skill_resources>") != std::string::npos);
            CHECK(pay.find("scripts/go.sh") != std::string::npos);
            CHECK(pay.find("</skill_content>") != std::string::npos);
        }
    }

    // ── Stage 5: activation is DERIVED from the visible transcript ───
    //
    // "Already active" used to be a process-global set (note_activated /
    // reset_activations). It is now a function of what the model can see.
    // Each check below is a case the stored set got wrong or needed a
    // hand-placed reset() for.
    {
        const auto body = [](const char* n) {
            return std::string{"<skill_content name=\""} + n + "\">\n...\n</skill_content>";
        };
        auto tool_result = [](std::string out) {
            ToolUse tc;
            tc.id     = ToolCallId{"call"};
            tc.name   = ToolName{"skill"};
            tc.status = ToolUse::Done{.output = std::move(out)};
            Message a; a.role = Role::Assistant;
            a.tool_calls.push_back(std::move(tc));
            return a;
        };

        // Nothing loaded → nothing active. (No reset() needed first, because
        // there is no state to reset.)
        Thread t;
        CHECK(!skills::is_active_in("alpha", visible_text(t)));

        // A body in a TOOL RESULT is active — that is where the skill tool
        // puts it, so tool outputs must be part of what is "visible".
        t.messages.push_back(tool_result(body("alpha")));
        CHECK(skills::is_active_in("alpha", visible_text(t)));
        CHECK(!skills::is_active_in("beta", visible_text(t)));   // independent

        // A prefix of another skill's name is NOT a match: the tag includes
        // the closing quote, so "review" does not light up "review-pr".
        t.messages.push_back(tool_result(body("review-pr")));
        CHECK(skills::is_active_in("review-pr", visible_text(t)));
        CHECK(!skills::is_active_in("review", visible_text(t)));

        // active_in lists each active name once, from tool results and text.
        {
            Message u; u.role = Role::User; u.text = body("gamma");   // /gamma expanded
            t.messages.push_back(std::move(u));
            auto names = skills::active_in(visible_text(t));
            CHECK(names.size() == 3);
        }

        // THE BUG THE STORED SET HAD. After /compact the tool_result holding
        // "alpha" is summarised away. The old set still said "active", so the
        // model was told the body was "in an earlier tool_result" that no
        // longer existed and never got it back. Derived, it is NOT active.
        {
            Thread c = t;
            Thread::CompactionRecord rec;
            rec.up_to_index = 1;                // drops the alpha tool_result
            rec.summary     = "the user loaded a skill earlier";
            c.compactions.push_back(rec);
            CHECK(!skills::is_active_in("alpha", visible_text(c)));
            CHECK(skills::is_active_in("review-pr", visible_text(c)));   // still visible
        }

        // The other half: a thread whose body is still in view needs NO
        // reset to be correct on resume, and does not get the body twice.
        // (The old set started empty on resume and re-injected it.)
        CHECK(skills::is_active_in("review-pr", visible_text(t)));

        // The tool-side view: what the dispatch puts in the call's context.
        {
            CallContext ctx;
            ctx.active_skills = skills::active_in(visible_text(t));
            CHECK(ctx.skill_active("alpha"));
            CHECK(!ctx.skill_active("delta"));
        }
    }

    // ── Stage 5b: spec lint ───────────────────────────────────
    {
        // Clean skill → no diagnostics.
        const auto f = skills::find(::agentty::IoAccess::grant(), "full-meta");
        CHECK(f && skills::lint(*f).empty());
        // Violations → diagnostics fire (loading stayed lenient).
        skills::Skill bad;
        bad.name = "Bad--Name-";
        bad.description = "";
        auto diags = skills::lint(bad);
        CHECK(diags.size() >= 3);   // charset + double hyphen + edge + desc
        // name/dir mismatch caught.
        const auto mm = skills::find(::agentty::IoAccess::grant(), "othername");
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
        const auto n = skills::find(::agentty::IoAccess::grant(), "embedded-startup");
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
        const auto impl = skills::find(::agentty::IoAccess::grant(), "perf-alloc");
        CHECK(impl && impl->body == "IMPLICIT NAME BODY");
        const auto deep = skills::find(::agentty::IoAccess::grant(), "deep-a-b-c");
        CHECK(deep && deep->body == "BODY");
        // Hidden dirs never yield skills.
        CHECK(!skills::find(::agentty::IoAccess::grant(), "cache-junk").has_value());
        // Shallower beats deeper on a joined-name collision.
        const auto p = skills::find(::agentty::IoAccess::grant(), "pair");
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
        const auto a = skills::find(::agentty::IoAccess::grant(), "ambig-dup");
        CHECK(a.has_value());
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
        const auto c = skills::find(::agentty::IoAccess::grant(), "my-group-sub-dir");
        CHECK(c.has_value());
        CHECK(c && c->body == "CHARSET");
        // The raw, unsanitized form must not exist under any spelling.
        CHECK(!skills::find(::agentty::IoAccess::grant(), "My Group-Sub_Dir").has_value());
        for (const auto& sk : skills::all(::agentty::IoAccess::grant())) {
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
        const auto c = skills::find(::agentty::IoAccess::grant(), "odd-name");
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
            const auto l = skills::find(::agentty::IoAccess::grant(), "aliased");
            CHECK(l.has_value());
            CHECK(l && l->body == "LINKED");
            // The resolved path must never leak into the name.
            CHECK(!skills::find(::agentty::IoAccess::grant(), "real-skill-dir").has_value());
            for (const auto& sk : skills::all(::agentty::IoAccess::grant()))
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
        for (const auto& sk : skills::all(::agentty::IoAccess::grant()))
            if (sk.description == "past the cap") ++past_cap;
        CHECK(past_cap == 0);
        // …and every name that IS discovered is typeable.
        for (const auto& sk : skills::all(::agentty::IoAccess::grant()))
            CHECK(sk.name.size() <= skills::kMaxSlugLen);
    }

    // A nested skill is NOT a resource of the skill it sits inside.
    write_file_at(work / ".agentty/skills/host/SKILL.md",
        "---\nname: host\ndescription: holds a nested skill\n---\nHOST BODY\n");
    write_file_at(work / ".agentty/skills/host/nested/inner/SKILL.md",
        "---\nname: host-nested-inner\ndescription: inside host\n---\nINNER\n");
    write_file_at(work / ".agentty/skills/host/refs/NOTE.md", "plain ref\n");
    {
        const auto h = skills::find(::agentty::IoAccess::grant(), "host");
        CHECK(h && h->resources.size() == 1);
        if (h && h->resources.size() == 1)
            CHECK(h->resources[0] == "refs/NOTE.md");
        CHECK(skills::find(::agentty::IoAccess::grant(), "host-nested-inner").has_value());
    }

    // Project shadows user with the same nested name.
    write_file_at(home / ".agentty/skills/embedded/startup/SKILL.md",
        "---\nname: embedded-startup\ndescription: user variant\n---\nUSER BODY\n");
    {
        const auto n = skills::find(::agentty::IoAccess::grant(), "embedded-startup");
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
        const auto u = skills::find(::agentty::IoAccess::grant(), "usr-res");   // discovery registers allowlist
        CHECK(u.has_value());
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
    util::set_workspace_root(::agentty::IoAccess::grant(), work);

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
    CHECK(skills::all(::agentty::IoAccess::grant()).size() == skills::kMaxSkills);

    // ── Raised: every discovered skill is catalogued, including the ones
    // the default-capped walk never even visited.
    set_env("200");
    CHECK(skills::all(::agentty::IoAccess::grant()).size() == kSkills);
    CHECK(skills::find(::agentty::IoAccess::grant(), "cap-skill-79").has_value());

    // ── Lowered: the catalog truncates to the override.
    set_env("10");
    CHECK(skills::all(::agentty::IoAccess::grant()).size() == 10);

    // ── Clamp floor: below the floor pins to it.
    set_env("2");
    CHECK(skills::all(::agentty::IoAccess::grant()).size() == 8);

    // ── Malformed: garbage keeps the default cap.
    set_env("not-a-number");
    CHECK(skills::all(::agentty::IoAccess::grant()).size() == skills::kMaxSkills);

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
        (void)skills::all(::agentty::IoAccess::grant());
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

// A SKILL.md too large to load must SAY so.
//
// read_capped returns empty for "over the cap" exactly as it does for
// "unreadable", so an oversized skill used to vanish from discovery with
// `0 warning(s)` -- the author's file sat on disk and nothing distinguished
// it from a path typo. Same shape as the shadow log: a file the catalog
// ignores is worse than one it rejects loudly.
TEST_CASE("skills: an oversized SKILL.md is reported, not dropped") {
    agtest::ScopedEnvSandbox _env_guard;
    std::error_code ec;
    fs::path base = fs::temp_directory_path(ec) / "agentty_skills_big_test";
    fs::remove_all(base, ec);
    fs::path home = base / "home";
    fs::path work = base / "work";
    fs::create_directories(home / ".agentty/skills", ec);
    fs::create_directories(work / ".agentty/skills", ec);
#if defined(_WIN32)
    _putenv_s("HOME", home.string().c_str());
    _putenv_s("AGENTTY_HOME", "");
#else
    setenv("HOME", home.string().c_str(), 1);
    unsetenv("AGENTTY_HOME");
#endif
    fs::current_path(work, ec);

    // One good skill beside one that is past the cap.
    write_file_at(work / ".agentty/skills/fine/SKILL.md",
                  "---\nname: fine\ndescription: loads normally\n---\nbody\n");
    {
        std::string big = "---\nname: toobig\ndescription: x\n---\n";
        big.append(skills::kMaxBodyBytes + 1024, 'x');
        write_file_at(work / ".agentty/skills/toobig/SKILL.md", big);
    }

    const auto& all = skills::all(::agentty::IoAccess::grant());
    const skills::Skill* fine = nullptr;
    const skills::Skill* big  = nullptr;
    for (const auto& s : all) {
        if (s.name == "fine")   fine = &s;
        if (s.name == "toobig") big  = &s;
    }

    REQUIRE(fine != nullptr);
    CHECK(fine->oversized_bytes == 0, "a normal skill is not flagged");

    REQUIRE_MESSAGE(big != nullptr,
        "an oversized SKILL.md must still appear in discovery -- silently "
        "dropping it is how an author loses a skill with no way to tell");
    CHECK(big->oversized_bytes > skills::kMaxBodyBytes,
          "the measured size is carried for the warning");

    // It must LINT, and say the one useful thing rather than a pile of
    // "description is missing" from fields that were never parsed.
    const auto warns = skills::lint(*big);
    REQUIRE(!warns.empty(), "it produces a warning");
    CHECK(warns.size() == 1, "exactly one warning -- the relevant one");
    CHECK(warns.front().find("NOT loaded") != std::string::npos,
          "the warning says the skill is not loaded");

    // And it must NOT reach the model: there is no body to activate, so
    // offering it would buy a wasted turn.
    const std::string catalog = skills::catalog_block(::agentty::IoAccess::grant());
    CHECK(catalog.find("toobig") == std::string::npos,
          "an unloadable skill is never offered to the model");
    CHECK(catalog.find("fine") != std::string::npos,
          "the good skill beside it is unaffected");

    fs::current_path(base, ec);
    fs::remove_all(base, ec);
}
