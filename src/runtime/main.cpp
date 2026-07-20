// agentty — terminal Claude Code clone built on maya.
//
// main.cpp is wiring only:
//   1. parse argv (subcommands + options)
//   2. resolve credentials
//   3. construct the Provider + Store satisfying the io concepts
//   4. install the Deps so update/cmd_factory can reach them
//   5. hand AgenttyApp to maya's runtime

// Route global operator new/delete through mimalloc. Must live in exactly
// one TU of the final executable — main.cpp is the natural home. Enabled
// by -DAGENTTY_USE_MIMALLOC=ON at configure time (default ON, silently off if
// the package isn't available).
#if defined(AGENTTY_USE_MIMALLOC)
#  include <mimalloc-new-delete.h>
#endif

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#  include <mmsystem.h>          // timeBeginPeriod / timeEndPeriod
#  if defined(_MSC_VER)
//   MSVC consumes the pragma and links winmm.lib automatically. GCC
//   ignores it with a warning — we link winmm via target_link_libraries
//   in CMakeLists.txt for the MinGW build, so the pragma is pointless
//   noise there.
#    pragma comment(lib, "winmm.lib")
#  endif
#endif

#include <cstdio>
#include <iostream>
#include <string>
#include <utility>

#include <maya/maya.hpp>

#include "agentty/acp/server.hpp"
#include "agentty/airgap/airgap.hpp"
#include "agentty/domain/profile.hpp"
#include "agentty/runtime/app/deps.hpp"
#include "agentty/runtime/app/program.hpp"
#include "agentty/auth/auth.hpp"
#include "agentty/io/persistence.hpp"
#include "agentty/mcp/serve.hpp"
#include "agentty/rag/bench.hpp"
#include "agentty/provider/anthropic/provider.hpp"
#include "agentty/provider/openai/provider.hpp"
#include "agentty/provider/ollama/provider.hpp"
#include "agentty/provider/selection.hpp"
#include "agentty/tool/skills.hpp"
#include "agentty/tool/util/fs_helpers.hpp"
#include "agentty/tool/util/sandbox.hpp"
#include "agentty/tool/subagent.hpp"
#include "agentty/tool/mcp_tools_bridge.hpp"

namespace {

// Compiled-in project version — populated by CMakeLists.txt's
// target_compile_definitions(agentty PRIVATE AGENTTY_VERSION=...). The fallback
// "0.0.0-dev" is only reached on a build that bypasses our CMake (e.g.
// hand-invoked compiler), which keeps the binary self-describing instead
// of a hard #error.
#ifndef AGENTTY_VERSION
#define AGENTTY_VERSION "0.0.0-dev"
#endif

void print_version() {
    std::printf("agentty %s\n", AGENTTY_VERSION);
}

void print_usage() {
    std::fprintf(stderr,
        "agentty %s\n"
        "\n"
        "usage: agentty [subcommand] [options]\n"
        "\n"
        "subcommands:\n"
        "  login             Authenticate (API key, or OAuth via claude.ai)\n"
        "  logout            Remove saved credentials\n"
        "  status            Show current auth status\n"
        "  airgap            Launch agentty on an air-gapped host via SSH tunnel\n"
        "                    (`agentty airgap --help` for details)\n"
        "  acp               Run as an ACP agent over stdio (for Zed et al.)\n"
        "  mcp-serve         Serve agentty's native tools over MCP (stdio).\n"
        "                    Point any MCP client at `agentty mcp-serve`.\n"
        "  skills            List discovered skills with spec-lint diagnostics\n"
        "                    (exit 1 on warnings — CI-friendly validate)\n"
        "  rag-bench [dir]   Benchmark search_docs retrieval on your own corpus\n"
        "                    (recall@k / MRR / nDCG per pipeline stage)\n"
        "  version           Print the agentty version and exit\n"
        "  help              Show this message\n"
        "\n"
        "options:\n"
        "  -k, --key KEY       API-key override for this session\n"
        "  -m, --model ID      Model id (e.g. claude-opus-4-5)\n"
        "  -w, --workspace DIR Sandbox filesystem tools to this directory\n"
        "                      (default: cwd). Tools refuse paths outside it.\n"
        "                      Pass `--workspace /` to disable the gate.\n"
        "      --sandbox MODE  Wrap bash/diagnostics in an OS-native sandbox\n"
        "                      (Linux: bwrap, macOS: sandbox-exec).\n"
        "                      MODE = auto (default: use if available),\n"
        "                             on  (require backend; fail otherwise),\n"
        "                             off (disable wrapping).\n"
        "  -p, --profile MODE  ACP permission tier (Zed shows the prompts):\n"
        "                             ask     (default: prompt write/exec/net),\n"
        "                             minimal (also prompt reads),\n"
        "                             write   (never prompt reads).\n"
        "      --provider P    LLM backend. anthropic (default, OAuth/Pro/Max)\n"
        "                      or an OpenAI-compatible one: openai | groq |\n"
        "                      openrouter | together | cerebras | ollama |\n"
        "                      llama.cpp, or a raw host[:port] for any other\n"
        "                      OpenAI-compatible server. Reads OPENAI_API_KEY\n"
        "                      (or the provider-specific *_API_KEY) / -k for\n"
        "                      the key; local backends need no key. Persisted\n"
        "                      like -m. (Switch live in-app with Ctrl-P \xe2\x80\x94 the\n"
        "                      picker has a \"Custom host\xe2\x80\xa6\" entry too.)\n"
        "  -V, --version       Print the agentty version and exit.\n"
        "      --auth-header N Auth header NAME for OpenAI-compatible backends\n"
        "                      whose gateway doesn't accept `Authorization:\n"
        "                      Bearer` (e.g. X-API-Key). The key (-k /\n"
        "                      OPENAI_API_KEY) is sent raw under that name.\n"
        "                      Session-scoped like -k; default: Bearer.\n"
        "  -h, --help          Show this message.\n"
        "\n",
        AGENTTY_VERSION);
}

struct Args {
    std::string subcommand;
    std::string cli_key;
    std::string cli_model;
    std::string cli_workspace;
    std::string cli_sandbox;   // "auto" | "on" | "off"; empty = auto default
    std::string cli_profile;   // "write" | "ask" | "minimal"; ACP only
    std::string cli_provider;  // "anthropic" | "openai" | "ollama" | "llama.cpp" | host[:port]
    std::string cli_auth_header; // custom auth header NAME (e.g. "X-API-Key")
    std::string cli_bench_root;  // rag-bench: docs root override (positional)
    int         airgap_argc = 0;
    char**      airgap_argv = nullptr;   // borrowed from main's argv
    bool        bad = false;
};

Args parse_args(int argc, char** argv) {
    Args out;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "login" || a == "logout" || a == "status" || a == "help"
         || a == "acp" || a == "skills" || a == "mcp-serve") {
            out.subcommand = std::move(a);
        } else if (a == "rag-bench") {
            // Optional positional docs root: `agentty rag-bench [dir]`.
            out.subcommand = std::move(a);
            if (i + 1 < argc && argv[i + 1][0] != '-')
                out.cli_bench_root = argv[++i];
        } else if (a == "airgap") {
            // Hand the remaining argv tail to the airgap subcommand verbatim
            // so it can run its own flag parsing without re-implementing
            // ours.  Stop scanning — top-level flags don't apply.
            out.subcommand   = std::move(a);
            out.airgap_argc  = argc - (i + 1);
            out.airgap_argv  = argv + (i + 1);
            return out;
        } else if ((a == "-k" || a == "--key") && i + 1 < argc) {
            out.cli_key = argv[++i];
        } else if ((a == "-m" || a == "--model") && i + 1 < argc) {
            out.cli_model = argv[++i];
        } else if ((a == "-w" || a == "--workspace") && i + 1 < argc) {
            out.cli_workspace = argv[++i];
        } else if (a == "--sandbox" && i + 1 < argc) {
            out.cli_sandbox = argv[++i];
        } else if ((a == "-p" || a == "--profile") && i + 1 < argc) {
            out.cli_profile = argv[++i];
        } else if (a == "--provider" && i + 1 < argc) {
            out.cli_provider = argv[++i];
        } else if (a == "--auth-header" && i + 1 < argc) {
            out.cli_auth_header = argv[++i];
        } else if (a == "-h" || a == "--help") {
            out.subcommand = "help";
        } else if (a == "-V" || a == "--version" || a == "version") {
            // Standalone version subcommand / flag. Treated as a
            // top-level dispatch path so it short-circuits the rest
            // of argparse — `agentty --version -k garbage` shouldn't
            // complain about the unused -k.
            out.subcommand = "version";
            return out;
        } else {
            std::fprintf(stderr, "unknown arg: %s\n\n", a.c_str());
            out.bad = true;
            return out;
        }
    }
    return out;
}

} // namespace

#if defined(_WIN32)
// RAII guard for Windows-specific tuning that must be undone at process exit:
//   - timeBeginPeriod(1): bumps the system-wide timer interrupt from the
//     default 15.625 ms down to 1 ms. Every Sleep, WaitForSingleObject
//     timeout, and std::this_thread::sleep_for respects this floor, so
//     spinner ticks / streaming frame pacing / input-poll cadence become
//     smooth instead of stepping on a ~16 ms grid. The effect is global,
//     so we must pair it with timeEndPeriod(1) on teardown.
//   - SetPriorityClass(ABOVE_NORMAL): interactive TUI — we want our
//     render/input loop to preempt background compilation or Slack over
//     the user's CPU. Doesn't affect a quiescent process; only buys
//     contention-time responsiveness.
struct Win32PerfTuning {
    bool hi_res_timer = false;
    Win32PerfTuning() {
        if (::timeBeginPeriod(1) == TIMERR_NOERROR) hi_res_timer = true;
        ::SetPriorityClass(::GetCurrentProcess(), ABOVE_NORMAL_PRIORITY_CLASS);
    }
    ~Win32PerfTuning() {
        if (hi_res_timer) ::timeEndPeriod(1);
    }
};
#endif

int main(int argc, char** argv) {
    using namespace agentty;

#if defined(_WIN32)
    Win32PerfTuning win32_perf;
#endif

    auto args = parse_args(argc, argv);
    if (args.bad)                    { print_usage(); return 2; }
    if (args.subcommand == "help")    { print_usage();   return 0; }
    if (args.subcommand == "version") { print_version(); return 0; }
    if (args.subcommand == "login")  return auth::cmd_login();
    if (args.subcommand == "logout") return auth::cmd_logout();
    if (args.subcommand == "status") return auth::cmd_status();
    if (args.subcommand == "skills") return tools::skills::cmd_skills();
    if (args.subcommand == "rag-bench")
        return rag::bench::run(args.cli_bench_root);
    if (args.subcommand == "airgap")
        return airgap::cmd_airgap(args.airgap_argc, args.airgap_argv);

    // Missing creds is no longer a fatal error: we install with an empty
    // auth header, init.cpp opens the in-app login modal, and the user
    // finishes signing in inside the TUI. The reducer's LoginExchanged /
    // LoginSubmit handlers call auth::update_auth() which live-swaps the
    // creds in the Deps without requiring a process restart.
    auto creds = auth::resolve(args.cli_key);

    // Persist -m as the new default — but NOT in ACP mode, where the model
    // is an ephemeral per-subprocess override (handled below) that must not
    // clobber the TUI's saved model.
    if (!args.cli_model.empty() && args.subcommand != "acp") {
        auto s = persistence::load_settings();
        s.model_id = ModelId{args.cli_model};
        persistence::save_settings(s);
    }

    // ── Filesystem sandbox boundary ─────────────────────────────────────
    // Default workspace = process cwd. Tools that touch the filesystem
    // (read/write/edit/list_dir/grep/glob/find_definition/git_*/bash's
    // `cd`) refuse paths outside this root with a clear error. Pass
    // `--workspace <dir>` to widen — `--workspace /` disables the gate
    // entirely for users who explicitly want unrestricted access.
    if (!args.cli_workspace.empty()) {
        std::filesystem::path req{args.cli_workspace};
        std::error_code ec;
        if (!std::filesystem::is_directory(req, ec)) {
            std::fprintf(stderr,
                "agentty: --workspace path is not a directory: %s\n",
                args.cli_workspace.c_str());
            return 2;
        }
        tools::util::set_workspace_root(std::move(req));
    } else {
        std::error_code ec;
        auto cwd = std::filesystem::current_path(ec);
        if (!ec) tools::util::set_workspace_root(std::move(cwd));
    }

    // ── Bash / diagnostics sandbox ──────────────────────────────────────
    // Wraps shell commands in bwrap (Linux) or sandbox-exec (macOS) so an
    // approved bash call can't read ~/.ssh, write /etc, or `rm -rf ~`.
    // `auto` (default): use if available, log warning otherwise. `on`:
    // fail loud if the backend is missing — for users who'd rather not
    // run unsandboxed at all. `off`: explicit opt-out for environments
    // where the user has external isolation (Docker, VM, whatever).
    {
        auto mode = tools::util::sandbox::Mode::Auto;
        if (args.cli_sandbox == "off")       mode = tools::util::sandbox::Mode::Off;
        else if (args.cli_sandbox == "on")   mode = tools::util::sandbox::Mode::On;
        else if (args.cli_sandbox == "auto"
              || args.cli_sandbox.empty())   mode = tools::util::sandbox::Mode::Auto;
        else {
            std::fprintf(stderr,
                "agentty: --sandbox must be auto, on, or off (got '%s')\n",
                args.cli_sandbox.c_str());
            return 2;
        }
        bool ok = tools::util::sandbox::init(mode);
        if (!ok) {
            std::fprintf(stderr,
                "agentty: --sandbox=on but no backend available. %s\n",
                tools::util::sandbox::describe_state().c_str());
            return 2;
        }
        // Status line so the user knows what they got. Stdout is fine —
        // maya runs after this returns, no clobbering.
        std::fprintf(stderr, "agentty: %s\n",
                     tools::util::sandbox::describe_state().c_str());
    }

    // ── Mirror the tool runtime into mcp-cpp ────────────────────────────
    // The local tool set is now served by mcp-cpp's batteries-included
    // toolset (see build_registry / mcp_tools_bridge). Mirror agentty's
    // workspace-root boundary + sandbox mode into mcp's util layer so the
    // bridged read/write/edit/bash/git tools enforce the SAME --workspace
    // gate and bwrap/sandbox-exec isolation the native tools did. Must run
    // before any tool can dispatch (TUI, ACP, and mcp-serve all reach this).
    tools::wire_mcp_runtime(args.cli_sandbox);

    // ── Resolve the active provider ──────────────────────────────
    // --provider wins; otherwise the saved setting; otherwise Anthropic.
    // "anthropic" (default) keeps the OAuth/Pro/Max path. Any other value
    // ("openai" | "groq" | "openrouter" | "ollama" | "host[:port]") routes
    // through the OpenAI-compatible transport.
    std::string provider_spec = args.cli_provider;
    if (provider_spec.empty()) {
        auto s = persistence::load_settings();
        provider_spec = s.provider;          // empty → anthropic
    } else if (args.subcommand != "acp") {
        // Persist the --provider choice (except in ACP mode, where it's an
        // ephemeral per-subprocess override like -m). When no -m was given,
        // also restore the model last used on THIS provider so `--provider X`
        // boots straight onto X's model instead of carrying a model id that
        // belongs to a different backend (and 400s on the first prompt).
        auto s = persistence::load_settings();
        s.provider = provider_spec;
        if (args.cli_model.empty()) {
            if (auto it = s.provider_models.find(provider_spec);
                it != s.provider_models.end() && !it->second.empty())
                s.model_id = ModelId{it->second};
        } else {
            // -m given alongside --provider: file the model as this
            // provider's recall so a later bare relaunch restores it.
            s.provider_models[provider_spec] = args.cli_model;
        }
        persistence::save_settings(s);
    }
    // Session-scoped --auth-header override; must be installed BEFORE
    // parse_selection so the initial Selection (and every live-switch
    // rebuild) stamps it onto the OpenAI-family Endpoint.
    provider::set_custom_auth_header(args.cli_auth_header);
    auto selection = provider::parse_selection(provider_spec);
    provider::select(selection);

    // Auth header per provider, registry-driven. Anthropic uses the
    // OAuth/key creds resolved above; OpenAI-family backends read their
    // provider-specific env var (GROQ_API_KEY, …) then OPENAI_API_KEY, or
    // -k; local backends (Ollama) accept an empty key. See
    // provider::resolve_auth_for — the single place that knows this mapping.
    auth::AuthHeader anthropic_creds = auth::make_auth_header(creds);
    std::string saved_provider_key;
    {
        auto s = persistence::load_settings();
        if (auto it = s.provider_keys.find(provider_spec);
            it != s.provider_keys.end())
            saved_provider_key = it->second;
    }
    auth::AuthHeader provider_auth =
        provider::resolve_auth_for(provider_spec, anthropic_creds,
                                   args.cli_key, saved_provider_key);

    // ── Wire the Provider + Store seams ─────────────────────────────────
    // Both providers live on main's stack so whichever the install lambda
    // captures by reference outlives maya::run / the ACP serve loop.
    provider::anthropic::AnthropicProvider anthropic_provider;
    io::FsStore                            store;

    // The seam: a single std::function the runtime calls. It dispatches on
    // provider::active() AT CALL TIME (not on a frozen branch), so the
    // provider picker can live-switch the backend mid-session
    // (provider::select() + app::switch_provider()) and the very next
    // request targets the new provider — no seam rebuild, no restart. For an
    // OpenAI-family switch we rebuild the per-call endpoint from the active
    // selection so a host/path/tls change takes effect immediately.
    std::function<void(provider::Request, provider::EventSink)> stream_fn =
        [&anthropic_provider]
        (provider::Request req, provider::EventSink sink) {
            const auto& sel = provider::active();
            if (sel.kind == provider::Kind::OpenAI) {
                // Ollama speaks its own native /api/chat dialect — route it to
                // the dedicated provider (structured tool_calls, keep_alive,
                // num_predict). Every other OpenAI-family backend uses the
                // compat /v1/chat/completions transport.
                if (sel.openai_endpoint.native_api) {
                    provider::ollama::OllamaProvider p{sel.openai_endpoint};
                    p.stream(std::move(req), std::move(sink));
                } else {
                    provider::openai::OpenAIProvider p{sel.openai_endpoint};
                    p.stream(std::move(req), std::move(sink));
                }
            } else {
                anthropic_provider.stream(std::move(req), std::move(sink));
            }
        };
    app::install_deps(app::Deps{
        .stream        = stream_fn,
        .save_thread   = [&store](const Thread& t) { store.save_thread(t); },
        .load_threads  = [&store] { return store.load_threads(); },
        .load_thread   = [&store](const ThreadId& id) { return store.load_thread(id); },
        .load_settings = [&store] { return store.load_settings(); },
        .save_settings = [&store](const store::Settings& x) { store.save_settings(x); },
        .new_thread_id = [&store] { return store.new_id(); },
        .title_from    = [&store](std::string_view t) { return store.title_from(t); },
        .auth          = provider_auth,
    });

    // ── Wire the subagent (`task` tool) seam ────────────────────
    // Process-global config the `task` tool reads to spin up an isolated
    // sub-agent loop. Resolve the model the same way the TUI / ACP paths
    // do (-m override → saved setting → built-in default). The stream fn
    // is the SAME provider dispatch Deps::stream uses (routes on
    // provider::active() at call time), so subagents work on Anthropic,
    // OpenAI-compat, and Ollama alike — not just the Anthropic transport.
    {
        auto sa_settings = persistence::load_settings();
        std::string sa_model =
            !args.cli_model.empty()       ? args.cli_model
          : !sa_settings.model_id.empty() ? sa_settings.model_id.value
          :                                 std::string{"claude-opus-4-5"};
        tools::subagent::install(tools::subagent::Config{
            provider_auth, std::move(sa_model), true, stream_fn});
    }

    // ── MCP server mode: serve agentty's native tools over MCP (stdio) ──
    // No maya, no terminal UI. An external MCP client (Claude Desktop, an
    // IDE, another agent) drives tools/list + tools/call over stdin/stdout;
    // diagnostics go to stderr. Reuses the provider/subagent/sandbox seams
    // wired above, so `bash`, `task`, the git_* family, etc. behave exactly
    // as they do in the TUI (filesystem tools stay sandboxed to --workspace).
    if (args.subcommand == "mcp-serve") {
        auth::prewarm_anthropic();   // `task`/web tools reuse the warm session
        int rc = mcp::serve_stdio();
        persistence::flush_pending_saves();
        return rc;
    }

    // ── ACP mode: run as a headless agent over stdio (Zed et al.) ───────
    // No maya, no terminal UI. stdin/stdout carry newline-delimited
    // JSON-RPC; all diagnostics go to stderr so the protocol channel stays
    // clean. Reuses the same provider/tools/sandbox wired above.
    if (args.subcommand == "acp") {
        auto settings = persistence::load_settings();
        // -m wins for this subprocess WITHOUT persisting to settings (an ACP
        // agent shouldn't clobber the TUI's saved model). Otherwise fall back
        // to the saved model, then the built-in default.
        std::string model_id =
            !args.cli_model.empty()    ? args.cli_model
          : !settings.model_id.empty() ? settings.model_id.value
          :                              std::string{"claude-opus-4-5"};

        // Prewarm TLS/DNS to api.anthropic.com so the first prompt reuses the
        // SSL session + connection cache instead of paying the ~150–300 ms
        // handshake. The TUI does the same before launching maya.
        auth::prewarm_anthropic();

        // Permission profile gates which tools trigger a Zed approval prompt.
        // Default Ask: prompt for write/exec/net, auto-run reads. `minimal`
        // also prompts for reads; `write` never prompts for reads.
        Profile profile = Profile::Ask;
        if      (args.cli_profile == "write")   profile = Profile::Write;
        else if (args.cli_profile == "minimal") profile = Profile::Minimal;
        else if (args.cli_profile == "ask" || args.cli_profile.empty())
                                                profile = Profile::Ask;
        else {
            std::fprintf(stderr,
                "agentty: --profile must be write, ask, or minimal (got '%s')\n",
                args.cli_profile.c_str());
            return 2;
        }

        ::acp::StdioTransport transport(std::cin, std::cout);
        agentty::acp::AgentServer server(
            transport,
            stream_fn,
            provider_auth,
            std::move(model_id),
            profile);
        std::fprintf(stderr, "agentty: ACP agent ready on stdio (profile=%s)\n",
                     std::string(to_string(profile)).c_str());
        int rc = server.serve();
        persistence::flush_pending_saves();
        return rc;
    }

    // Pre-warm TLS to api.anthropic.com on a detached background thread.
    // The first prompt the user types will reuse the SSL session + DNS +
    // connection cache, skipping ~150–300 ms of first-byte handshake.
    auth::prewarm_anthropic();

    // fps = 0 → pure event-driven: maya only renders on Msg / input / timer.
    // The spinner-tick subscription (gated on stream.active) supplies frames
    // while streaming; idle agentty costs zero CPU.
    maya::run<app::AgenttyApp>({.title = "agentty", .fps = 0, .mode = maya::Mode::Inline});

    // Drain the async persistence queue. The Quit reducer arm enqueues
    // a final save_thread() right before maya returns; this blocks
    // until that (and any earlier-still-queued) write lands on disk.
    persistence::flush_pending_saves();
    return 0;
}
