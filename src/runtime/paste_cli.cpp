// paste_cli.cpp — `agentty paste config …`: the paste.* settings from a shell.
//
// The same registry rows Settings → Paste shows, so a value set here is one
// the pane would accept (clamped, checked against its options) and lands in
// settings.json the same way. Everything else under `agentty paste` is
// richpaste's own command line.

#include "agentty/runtime/paste_cli.hpp"

#include "agentty/io/clipboard.hpp"
#include "agentty/io/persistence.hpp"
#include "agentty/runtime/settings_registry.hpp"

#include <cstdio>
#include <string>

namespace agentty::paste_cli {

namespace reg = settings::registry;

namespace {

// Accepts "max_mb" or "paste.max_mb".
const reg::SettingDef* find_row(std::string_view key) {
    std::string id{key};
    if (!id.starts_with("paste.")) id = "paste." + id;
    const auto* d = reg::find(id);
    return d && d->owner() == reg::Owner::Paste ? d : nullptr;
}

void list(const paste_cfg::Config& c, const reg::EnvSnapshot& env) {
    std::printf("paste settings (settings.json \"paste\", Settings \xe2\x86\x92 Paste)\n\n");
    for (const auto& d : reg::kSettings) {
        if (d.owner() != reg::Owner::Paste) continue;
        const std::string key{d.id.substr(6)};
        std::string val = reg::get(c, d);
        if (val.empty()) val = "\"\"";
        std::string where = reg::is_default(c, d) ? "default" : "settings.json";
        if (auto e = reg::env_override(d, env); !e.empty()) where = "env " + e;
        std::printf("  %-15s %-12s %-14s %s\n", key.c_str(), val.c_str(), where.c_str(),
                    std::string{d.help}.c_str());
    }
    std::printf("\nset:   agentty paste config set <key> <value>\n"
                "reset: agentty paste config reset <key|all>\n"
                "env:   each key also reads an override, which wins over settings.json:\n");
    for (const auto& d : reg::kSettings)
        if (d.owner() == reg::Owner::Paste)
            std::printf("         %-32s %s\n", std::string{d.env}.c_str(), std::string{d.id.substr(6)}.c_str());
}

int usage() {
    std::fprintf(stderr,
                 "usage: agentty paste config                 list every paste setting\n"
                 "       agentty paste config get <key>\n"
                 "       agentty paste config set <key> <value>\n"
                 "       agentty paste config reset <key|all>\n");
    return 2;
}

}  // namespace

int config(Io io, const maya::unlocked& cap, std::span<const std::string> args) {
    auto s = persistence::load_settings(io);
    const auto env = reg::read_env(io);
    if (args.empty() || args[0] == "list") {
        list(s.paste, env);
        return 0;
    }
    const std::string& verb = args[0];
    if (verb == "get" && args.size() == 2) {
        const auto* d = find_row(args[1]);
        if (!d) { std::fprintf(stderr, "agentty paste: no setting %s\n", args[1].c_str()); return 1; }
        std::printf("%s\n", reg::get(s.paste, *d).c_str());
        return 0;
    }
    if (verb == "set" && (args.size() == 3 || args.size() == 2)) {
        const auto* d = find_row(args[1]);
        if (!d) { std::fprintf(stderr, "agentty paste: no setting %s\n", args[1].c_str()); return 1; }
        const std::string value = args.size() == 3 ? args[2] : std::string{};
        if (!reg::set(s.paste, *d, value)) {
            std::fprintf(stderr, "agentty paste: %s can't be \"%s\"", std::string{d->id}.c_str(), value.c_str());
            if (!d->options.empty()) std::fprintf(stderr, " (one of %s)", std::string{d->options}.c_str());
            std::fprintf(stderr, "\n");
            return 1;
        }
        persistence::save_settings(io, cap, s);
        std::printf("%s = %s\n", std::string{d->id}.c_str(), reg::get(s.paste, *d).c_str());
        if (auto e = reg::env_override(*d, env); !e.empty())
            std::printf("note: %s is set, so it still wins over this\n", e.c_str());
        return 0;
    }
    if (verb == "reset" && args.size() == 2) {
        if (args[1] == "all") {
            s.paste = paste_cfg::Config{};
        } else {
            const auto* d = find_row(args[1]);
            if (!d) { std::fprintf(stderr, "agentty paste: no setting %s\n", args[1].c_str()); return 1; }
            reg::reset(s.paste, *d);
        }
        persistence::save_settings(io, cap, s);
        std::printf("reset %s\n", args[1].c_str());
        return 0;
    }
    return usage();
}

int run(Io io, const maya::unlocked& cap, std::span<const std::string> args) {
    if (!args.empty() && args[0] == "config") return config(io, cap, args.subspan(1));
    clipboard_configure(persistence::load_settings(io).paste);
    return clipboard_cli(args);
}

}  // namespace agentty::paste_cli
