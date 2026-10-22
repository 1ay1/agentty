// Paste pane: Settings → Paste drives m.d.persisted.paste, a committed edit
// is saved, and publish_derived hands the new value to the clipboard reader.
// Also pins the paste.* registry rows and the settings.json round trip.

#include "agtest.hpp"
#include "agtest_fx.hpp"

#include "agentty/io/clipboard.hpp"
#include "agentty/runtime/app/update.hpp"
#include "agentty/runtime/app/update/internal.hpp"
#include "agentty/runtime/composer_attachment.hpp"
#include "agentty/runtime/model.hpp"
#include "agentty/runtime/msg.hpp"
#include "agentty/runtime/panel/nav.hpp"
#include "agentty/runtime/panel/settings/items.hpp"
#include "agentty/runtime/settings_registry.hpp"

#include <string>

using namespace agentty;
namespace pn  = agentty::ui::panel;
namespace reg = agentty::settings::registry;
namespace se  = agentty::settings;
namespace fx  = agtest::fx;

namespace {

Model send(Model m, Msg msg) {
    auto [mm, _] = app::update(std::move(m), std::move(msg));
    (void)_;
    return std::move(mm);
}

const pn::PasteSettings& pane_of(const Model& m) {
    const auto* o = m.ui.panel.get<pn::PasteSettings>();
    REQUIRE(o != nullptr);
    return *o;
}

Msg key(form::keys::Intent i) { return Msg{PasteSettingsKey{form::keys::Action{i, 0}}}; }

void focus_row(Model& m, std::string_view row_id) {
    for (int guard = 0; guard < 32; ++guard) {
        const auto* row = pane_of(m).form.focused();
        if (row && row->id == row_id) return;
        m = send(std::move(m), key(form::keys::Intent::MoveNext));
    }
    CHECK(false);   // not a paste row
}

}  // namespace

TEST_CASE("paste pane: the General list has a door that opens it") {
    Model m;
    const auto items = se::items_for(m, se::Category::General);
    const se::Item* door = nullptr;
    for (const auto& i : items)
        if (i.action == se::Action::OpenPaste) door = &i;
    REQUIRE(door != nullptr);
    CHECK(door->primary == "Paste");
    CHECK(door->secondary.find("laptop over ssh") != std::string::npos);
    CHECK(se::opens_pane(door->action));

    m = send(std::move(m), Msg{OpenPasteSettings{}});
    const auto& f = pane_of(m).form;
    for (const auto& d : reg::kSettings)
        if (d.owner() == reg::Owner::Paste)
            CHECK(f.find(d.id) != nullptr, "every paste.* row is in the pane");
}

TEST_CASE("paste pane: a toggle persists and publishes") {
    Model m = send(Model{}, Msg{OpenPasteSettings{}});
    m.published_paste = m.d.persisted.paste;   // as if published at startup
    focus_row(m, "paste.display");
    auto [m2, cmd] = app::update(std::move(m), key(form::keys::Intent::Activate));
    CHECK(m2.d.persisted.paste.display == false);
    CHECK(fx::count<agentty::SaveSettings>(cmd) == 1);
    const auto* pub = fx::find<agentty::PublishPasteConfig>(cmd);
    REQUIRE(pub != nullptr);
    CHECK(pub->config.display == false);
}

TEST_CASE("paste pane: Esc closes it, and close_msg names it") {
    const Msg cm = pn::close_msg(pn::Kind::PasteSettings);
    CHECK(std::holds_alternative<msg::PasteSettingsMsg>(cm));
    Model m = send(Model{}, Msg{OpenPasteSettings{}});
    m = send(std::move(m), Msg{ClosePasteSettings{}});
    CHECK(m.ui.panel.get<pn::PasteSettings>() == nullptr);
}

TEST_CASE("paste settings: rows set, clamp and refuse like the pane") {
    paste_cfg::Config c;
    const auto* max = reg::find("paste.max_mb");
    REQUIRE(max != nullptr);
    CHECK(reg::set(c, *max, "99999"));
    CHECK(c.max_mb == paste_cfg::kMaxMbMax);
    const auto* term = reg::find("paste.terminal");
    REQUIRE(term != nullptr);
    CHECK_FALSE(reg::set(c, *term, "sometimes"));
    CHECK(reg::set(c, *term, "osc52"));
    CHECK(c.terminal == "osc52");
    CHECK_FALSE(reg::is_default(c, *term));
    reg::reset(c, *term);
    CHECK(c.terminal == "auto");
}

TEST_CASE("paste settings: decode_text off pastes richpaste lines as text") {
    const std::string line = "richpaste:1:image/png::12::iVBORw0KGgowMTIz";
    Model m;
    m.d.persisted.paste.decode_text = false;
    m = send(std::move(m), Msg{ComposerPaste{line}});
    // Not decoded: it goes the plain-text way (via the path check, which
    // answers "not an image file").
    if (m.ui.composer.attachments.empty())
        m = send(std::move(m), Msg{ImagePathSniffed{.text = line}});
    REQUIRE(m.ui.composer.attachments.size() == 1);
    CHECK(m.ui.composer.attachments[0].kind == Attachment::Kind::Paste);
    CHECK(m.ui.composer.attachments[0].body.bytes() == line);
}

TEST_CASE("paste: an unbracketed paste in pieces is one chip") {
    // Three reads of one paste: one chip, holding all of it.
    Model m;
    const std::string a(4095, 'x');
    m = send(std::move(m), Msg{ComposerPaste{a}});
    m = send(std::move(m), Msg{ComposerPaste{"yy", true, false, /*continues_paste=*/true}});
    m = send(std::move(m), Msg{ComposerPaste{"\nzz", true, false, /*continues_paste=*/true}});
    REQUIRE(m.ui.composer.attachments.size() == 1);
    const auto& att = m.ui.composer.attachments[0];
    CHECK(att.body.bytes() == a + "yy\nzz");
    CHECK(att.line_count == 2);
    CHECK(attachment::chip_label(att) == "Pasted text \xc2\xb7 2 lines \xc2\xb7 4 KB");

    // Typing after it is text, not more paste.
    m = send(std::move(m), Msg{ComposerCharInput{U'k'}});
    CHECK(m.ui.composer.text.back() == 'k');
    CHECK(m.ui.composer.attachments.size() == 1);
}

TEST_CASE("paste: an image sent as text, in pieces, becomes the image") {
    const std::string line = "richpaste:1:image/png::12::iVBORw0KGgowMTIz";
    Model m;
    m = send(std::move(m), Msg{ComposerPaste{line.substr(0, 30)}});
    REQUIRE(m.ui.composer.attachments.size() == 1);
    CHECK(m.ui.composer.attachments[0].kind == Attachment::Kind::Paste);
    m = send(std::move(m), Msg{ComposerPaste{line.substr(30), true, false, /*continues_paste=*/true}});
    // The paste chip is replaced by the image chip.
    bool image = false, paste_left = false;
    for (const auto& a : m.ui.composer.attachments) {
        if (a.kind == Attachment::Kind::Image) image = true;
        if (a.kind == Attachment::Kind::Paste && a.byte_count > 0) paste_left = true;
    }
    CHECK(image);
    CHECK_FALSE(paste_left);
    CHECK(m.ui.composer.text.find(attachment::make_placeholder(0)) == std::string::npos);
}

TEST_CASE("paste: a long single line shows its size, not a preview") {
    Attachment a;
    a.kind = Attachment::Kind::Paste;
    a.line_count = 1;
    a.byte_count = 3000;
    a.body.set_bytes(std::string(3000, 'Q'));
    CHECK(attachment::chip_label(a) == "Pasted text \xc2\xb7 1 line \xc2\xb7 2 KB");
    Attachment s;
    s.kind = Attachment::Kind::Paste;
    s.line_count = 1;
    s.byte_count = 5;
    s.body.set_bytes("hello");
    CHECK(attachment::chip_label(s) == "Pasted: hello");
}

TEST_CASE("paste settings: clipboard_configure installs them") {
    paste_cfg::Config c;
    c.inline_kb = 7;
    c.save_dir = "/tmp/agentty-paste-test-dir";
    clipboard_configure(c);
    CHECK(clipboard_config().inline_kb == 7);
    CHECK(clipboard_save_dir() == "/tmp/agentty-paste-test-dir");
    clipboard_configure(paste_cfg::Config{});   // leave defaults for the next test
    CHECK(clipboard_config() == paste_cfg::Config{});
}
