// moha — terminal Claude Code clone built on maya.
//
// main.cpp is wiring only:
//   1. parse argv (subcommands + options)
//   2. resolve credentials
//   3. construct the Provider + Store satisfying the io concepts
//   4. install the Deps so update/cmd_factory can reach them
//   5. hand MohaApp to maya's runtime

#include <cstdio>
#include <string>
#include <utility>

#include <maya/maya.hpp>

#include "moha/app/deps.hpp"
#include "moha/app/program.hpp"
#include "moha/io/auth.hpp"
#include "moha/io/anthropic_provider.hpp"
#include "moha/io/store.hpp"
#include "moha/io/persistence.hpp"

namespace {

void print_usage() {
    std::fprintf(stderr,
        "usage: moha [subcommand] [options]\n"
        "\n"
        "subcommands:\n"
        "  login             Authenticate (OAuth via claude.ai or API key)\n"
        "  logout            Remove saved credentials\n"
        "  status            Show current auth status\n"
        "  help              Show this message\n"
        "\n"
        "options:\n"
        "  -k, --key KEY     API-key override for this session\n"
        "  -m, --model ID    Model id (e.g. claude-opus-4-5)\n"
        "\n");
}

struct Args {
    std::string subcommand;
    std::string cli_key;
    std::string cli_model;
    bool        bad = false;
};

Args parse_args(int argc, char** argv) {
    Args out;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "login" || a == "logout" || a == "status" || a == "help") {
            out.subcommand = std::move(a);
        } else if ((a == "-k" || a == "--key") && i + 1 < argc) {
            out.cli_key = argv[++i];
        } else if ((a == "-m" || a == "--model") && i + 1 < argc) {
            out.cli_model = argv[++i];
        } else if (a == "-h" || a == "--help") {
            out.subcommand = "help";
        } else {
            std::fprintf(stderr, "unknown arg: %s\n\n", a.c_str());
            out.bad = true;
            return out;
        }
    }
    return out;
}

} // namespace

int main(int argc, char** argv) {
    using namespace moha;

    auto args = parse_args(argc, argv);
    if (args.bad)                    { print_usage(); return 2; }
    if (args.subcommand == "help")   { print_usage(); return 0; }
    if (args.subcommand == "login")  return auth::cmd_login();
    if (args.subcommand == "logout") return auth::cmd_logout();
    if (args.subcommand == "status") return auth::cmd_status();

    auto creds = auth::resolve(args.cli_key);
    if (!creds.is_valid()) {
        std::fprintf(stderr,
            "moha: not authenticated.\n"
            "  run:  moha login\n"
            "  or:   export ANTHROPIC_API_KEY=sk-ant-...\n"
            "  or:   export CLAUDE_CODE_OAUTH_TOKEN=...\n");
        return 1;
    }

    if (!args.cli_model.empty()) {
        auto s = persistence::load_settings();
        s.model_id = ModelId{args.cli_model};
        persistence::save_settings(s);
    }

    // ── Wire the Provider + Store seams ─────────────────────────────────
    io::AnthropicProvider provider;
    io::FsStore           store;
    app::install(provider, store, creds.header_value(), creds.style());

    maya::run<app::MohaApp>({.title = "moha", .fps = 30, .mode = maya::Mode::Inline});
    return 0;
}
