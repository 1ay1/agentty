// picker_sections_render_test — the fused model picker's SECTION HEADERS,
// asserted against real rendered cells.
//
// The three-section browse layout ("recent" / "from this provider" / "from
// all other providers") is decided in the builder but SPELLED in the view,
// and the spelling is conditional: "from all OTHER providers" only reads
// correctly when a "from this provider" section is on screen to be other
// than. On a fresh install no model is active, so no catalog is the active
// one, every row lands in that section — and the header pointed the user at
// a section that does not exist.
//
// That was previously guarded by grepping pickers.cpp for the string, which
// proves the branch was TYPED, not that it RENDERS. A source-text assertion
// cannot tell you the header is reachable, that the row it titles is on
// screen, or that the fallback fires for the right input. This renders the
// picker through the real view + maya and reads the header back off the
// canvas.

#include "agtest.hpp"

#include "agentty/runtime/model.hpp"
#include "agentty/runtime/fused_models.hpp"
#include "agentty/runtime/view/pickers.hpp"

#include <maya/render/canvas.hpp>
#include <maya/render/renderer.hpp>
#include <maya/style/theme.hpp>

#include <string>
#include <vector>

using namespace agentty;
namespace ov = agentty::ui::overlay;

namespace {

ModelInfo mk(std::string id, std::string name, std::string provider) {
    ModelInfo mi;
    mi.id = ModelId{std::move(id)};
    mi.display_name = std::move(name);
    mi.provider = std::move(provider);
    mi.context_window = 200000;
    return mi;
}

ProviderCatalog cat(std::string id, std::string label,
                    std::vector<ModelInfo> models) {
    ProviderCatalog c;
    c.provider_id = std::move(id);
    c.label = std::move(label);
    c.state = ProviderCatalog::State::Ready;
    c.models = std::move(models);
    return c;
}

// Render ONLY the picker element (not the whole app layout) and flatten the
// canvas to text. Non-ASCII folds to '?' — the section titles are ASCII, and
// the badge/■ chrome around them is not what we're asserting on.
std::string render_picker(const Model& m, int width = 100, int height = 200) {
    auto root = ui::fused_picker(m);
    maya::StylePool pool;
    maya::Canvas canvas(width, height, &pool);
    canvas.clear();
    maya::render_tree(root, canvas, pool, maya::theme::dark, true);

    std::string out;
    const int max_row = canvas.max_content_row();
    for (int y = 0; y <= max_row; ++y) {
        std::string line;
        for (int x = 0; x < width; ++x) {
            char32_t ch = canvas.get(x, y).character;
            if (ch == 0) ch = U' ';
            line.push_back(ch < 128 ? static_cast<char>(ch) : '?');
        }
        while (!line.empty() && line.back() == ' ') line.pop_back();
        out += line;
        out.push_back('\n');
    }
    return out;
}

bool has(const std::string& hay, std::string_view needle) {
    return hay.find(needle) != std::string::npos;
}

// Build a Model with the picker open over `cats`, as the reducer would:
// fused_rows is the reducer-maintained cache the view reads, never rebuilt
// per frame, so a render test must populate it the same way.
Model picker_model(const std::vector<ProviderCatalog>& cats,
                   ModelRef active = {}) {
    Model m;
    ui::FusedInputs in;
    in.catalogs = &cats;
    in.active   = active;
    m.d.fused_rows = ui::build_fused_rows(in);
    m.ui.overlay = ov::FusedPicker{};
    return m;
}

}  // namespace

TEST_CASE("picker: with an active provider, sections are titled by contrast") {
    // The active provider needs a SECOND model: the active one is pinned
    // into RECENT, so a single-model catalog contributes no non-recent rows
    // and the "from this provider" section would legitimately be absent.
    const std::vector<ProviderCatalog> cats{
        cat("anthropic", "Anthropic",
            {mk("claude-opus-4-5",  "Claude Opus 4.5",  "anthropic"),
             mk("claude-haiku-4-5", "Claude Haiku 4.5", "anthropic")}),
        cat("openai", "OpenAI", {mk("gpt-5", "GPT-5", "openai")}),
    };
    const Model m = picker_model(cats, ModelRef{"anthropic", "claude-opus-4-5"});
    const std::string screen = render_picker(m);

    INFO(screen);
    // Both halves of the contrast are on screen, so "other" has a referent.
    CHECK(has(screen, "from this provider"));
    CHECK(has(screen, "from all other providers"));
    // And the rows they title actually rendered.
    CHECK(has(screen, "Claude Haiku 4.5"));   // this provider, non-recent
    CHECK(has(screen, "GPT-5"));              // another provider
}

TEST_CASE("picker: with NO active provider, the header drops the contrast") {
    // The fresh-install shape: nothing active, so no catalog is "this"
    // provider and every row lands in the others section.
    const std::vector<ProviderCatalog> cats{
        cat("anthropic", "Anthropic",
            {mk("claude-opus-4-5", "Claude Opus 4.5", "anthropic")}),
        cat("openai", "OpenAI", {mk("gpt-5", "GPT-5", "openai")}),
    };
    const Model m = picker_model(cats);           // active left EMPTY
    const std::string screen = render_picker(m);

    INFO(screen);
    // There is no "this provider" block...
    CHECK_FALSE(has(screen, "from this provider"));
    // ...so the header must NOT ask the user to contrast against one.
    CHECK_FALSE(has(screen, "from all other providers"));
    // It says what the section actually holds instead.
    CHECK(has(screen, "all providers"));
    // The models are still listed — the fallback titles them, not hides them.
    CHECK(has(screen, "Claude Opus 4.5"));
    CHECK(has(screen, "GPT-5"));
}

TEST_CASE("picker: the active model leads, and its row is on screen") {
    // The ● pin is a builder invariant (fused_models_test proves the
    // ordering); this pins that the picker RENDERS it in the recent section
    // rather than ordering it correctly and then drawing something else.
    const std::vector<ProviderCatalog> cats{
        cat("anthropic", "Anthropic",
            {mk("claude-opus-4-5",  "Claude Opus 4.5",  "anthropic"),
             mk("claude-haiku-4-5", "Claude Haiku 4.5", "anthropic")}),
    };
    const std::vector<ModelRef> recents{
        ModelRef{"anthropic", "claude-haiku-4-5"},
        ModelRef{"anthropic", "claude-opus-4-5"},
    };
    Model m;
    ui::FusedInputs in;
    in.catalogs = &cats;
    in.recents  = &recents;
    in.active   = ModelRef{"anthropic", "claude-opus-4-5"};
    m.d.fused_rows = ui::build_fused_rows(in);
    m.ui.overlay = ov::FusedPicker{};

    const std::string screen = render_picker(m);
    INFO(screen);
    CHECK(has(screen, "recent"));
    // Haiku sorts before Opus alphabetically; the active Opus must still be
    // the first RECENT row, because the picker pre-selects row 0.
    const auto recent_hdr = screen.find("recent");
    const auto opus       = screen.find("Claude Opus 4.5");
    const auto haiku      = screen.find("Claude Haiku 4.5");
    REQUIRE(recent_hdr != std::string::npos);
    REQUIRE(opus  != std::string::npos);
    REQUIRE(haiku != std::string::npos);
    CHECK(opus < haiku);
}
