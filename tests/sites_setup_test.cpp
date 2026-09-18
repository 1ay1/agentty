#include "agtest.hpp"
#include "agentty/runtime/app/subscribe.hpp"
#include "agentty/runtime/app/update.hpp"
#include "agentty/runtime/model.hpp"
#include "agentty/tool/skills.hpp"
#include "agentty/util/user_root.hpp"

#include <filesystem>
#include <fstream>
#include <iterator>

namespace {
namespace fs = std::filesystem;
using namespace agentty;

std::string contents(const fs::path& path) {
    std::ifstream f(path);
    return {std::istreambuf_iterator<char>(f), {}};
}

Model step(Model m, Msg msg) {
    return app::update(std::move(m), std::move(msg)).first;
}

std::optional<Msg> key_message(const maya::Sub<Msg>& sub, maya::Key key) {
    if (const auto* on_key = std::get_if<maya::Sub<Msg>::OnKey>(&sub.inner))
        return on_key->filter(maya::KeyEvent{.key = key});
    if (const auto* batch = std::get_if<maya::Sub<Msg>::Batch>(&sub.inner))
        for (const auto& child : batch->subs)
            if (auto result = key_message(child, key)) return result;
    return std::nullopt;
}


}

TEST_CASE("sites setup: separate final screen, default off, and existing skill preservation") {
    agtest::ScopedEnvSandbox guard;
    const auto base = fs::temp_directory_path() / "agentty-sites-setup-test";
    fs::remove_all(base);
    fs::create_directories(base / "home");
    fs::create_directories(base / "project");
    setenv("HOME", (base / "home").string().c_str(), 1);
    const auto root = base / "profile";
    const std::string old_root = std::getenv("AGENTTY_HOME") ? std::getenv("AGENTTY_HOME") : "";
    struct RestoreRoot {
        std::string value;
        ~RestoreRoot() { if (value.empty()) unsetenv("AGENTTY_HOME"); else setenv("AGENTTY_HOME", value.c_str(), 1); }
    } restore{old_root};
    setenv("AGENTTY_HOME", root.string().c_str(), 1);
    fs::current_path(base / "project");

    const auto skill = root / "skills/sites/SKILL.md";
    Model m;
    m.ui.login = ui::login::Picking{};
    m.s.sites_setup_pending = true;
    m.s.threads_loading = true;
    m = step(std::move(m), Msg{ThreadsLoaded{{}}});
    CHECK(m.s.sites_setup_pending);
    CHECK(!fs::exists(skill));
    CHECK(tools::skills::find("sites") == nullptr);
    // Sites is not mixed into provider selection.
    auto old_choice = key_message(app::subscribe(m), maya::CharKey{U'7'});
    REQUIRE(old_choice.has_value());
    m = step(std::move(m), std::move(*old_choice));
    CHECK(std::holds_alternative<ui::login::Picking>(m.ui.login));

    // A completed first-run inference choice opens Sites as its own final step.
    m.ui.login = ui::login::Closed{};
    m.ui.panel.descend(ui::panel::Models{{0, ""}});
    m = step(std::move(m), Msg{CloseModels{}});
    CHECK(std::holds_alternative<ui::login::SitesSetup>(m.ui.login));
    CHECK(!m.s.sites_setup_pending);
    CHECK(!fs::exists(skill));

    // Skipping closes setup and writes nothing.
    m = step(std::move(m), Msg{LoginPickMethod{U'2'}});
    CHECK(std::holds_alternative<ui::login::Closed>(m.ui.login));
    CHECK(!fs::exists(skill));

    // Explicit enablement installs through the same final screen.
    m.ui.login = ui::login::SitesSetup{};
    m = step(std::move(m), Msg{LoginPickMethod{U'1'}});
    CHECK(std::holds_alternative<ui::login::Closed>(m.ui.login));
    REQUIRE(fs::exists(skill));

    CHECK(!fs::exists(base / "home/.agentty/skills/sites/SKILL.md"));
    const auto* loaded = tools::skills::find("sites");
    REQUIRE(loaded != nullptr);
    CHECK(loaded->description.find("email") != std::string::npos);
    CHECK(loaded->body.find("https://cohesivity.ai/offerings/") != std::string::npos);
    CHECK(tools::skills::catalog_block().find("sites —") != std::string::npos);
    CHECK(!fs::exists(root / "mcp.json"));
    CHECK(!fs::exists(root / "settings.json"));

    std::ofstream(skill) << "---\nname: sites\ndescription: my own skill\n---\nMy deployment instructions.\n";
    const auto custom = contents(skill);
    CHECK(tools::skills::install_sites().empty());
    CHECK(contents(skill) == custom);
    Model restarted;
    restarted.ui.login = ui::login::Picking{};
    restarted.s.sites_setup_pending = true;
    restarted = step(std::move(restarted), Msg{ThreadsLoaded{{}}});
    CHECK(!restarted.s.sites_setup_pending);

    // An unrecognized file is still the user's file, not ours to replace.
    std::ofstream(skill) << "unfinished local instructions";
    const auto unfinished = contents(skill);
    (void)tools::skills::install_sites();
    CHECK(contents(skill) == unfinished);

    fs::remove_all(root / "skills");
    fs::create_directories(root);
    std::ofstream(root / "skills") << "not a directory";
    CHECK(!tools::skills::install_sites().empty());
    CHECK(!fs::exists(skill));

    Model returning;
    returning.ui.login = ui::login::Picking{};
    returning.s.sites_setup_pending = true;
    Thread previous;
    previous.id = ThreadId{"previous"};
    returning = step(std::move(returning), Msg{ThreadsLoaded{{previous}}});
    CHECK(!returning.s.sites_setup_pending);
    returning = step(std::move(returning), Msg{LoginPickMethod{U'7'}});
    CHECK(!fs::exists(skill));

    Model failing;
    failing.ui.login = ui::login::SitesSetup{};
    failing = step(std::move(failing), Msg{LoginPickMethod{U'1'}});
    CHECK(std::holds_alternative<ui::login::SitesSetup>(failing.ui.login));
    CHECK(!std::get<ui::login::SitesSetup>(failing.ui.login).error.empty());
    CHECK(!fs::exists(skill));

    Model account;
    account.ui.login = ui::login::Picking{.provider = "anthropic"};
    account = step(std::move(account), Msg{ThreadsLoaded{{}}});
    CHECK(!account.s.sites_setup_pending);
    fs::current_path(base.parent_path());
    fs::remove_all(base);
}
