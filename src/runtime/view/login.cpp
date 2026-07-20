// In-app login modal.
//
// Five sub-states keyed off `ui::login::State`:
//
//   Picking          choose OAuth (1) or API key (2)
//   OAuthCode        browser opened; user pastes callback code.
//                    The authorize URL gets a dedicated bordered box
//                    + `[c] copy URL` / `[o] open in browser again`
//                    affordances so the URL is one keystroke from the
//                    user's clipboard — no terminal mouse-select needed.
//   OAuthExchanging  HTTP POST in flight.
//   ApiKeyInput      paste an sk-ant-… key.
//   Failed           error toast above the Picking panel.
//
// Sizing is responsive: the outer wrapper sets a `min_width` floor
// and the Overlay widget's default Stretch lets it fill the available
// width (minus 2-col edge padding). Every text node uses TextWrap::Wrap
// so long URLs / API key labels reflow rather than truncate. Mirrors
// the picker chrome (see view/pickers.cpp's wrap_picker).

#include "agentty/runtime/view/login.hpp"

#include <string>
#include <variant>
#include <vector>

#include "agentty/runtime/login.hpp"
#include "agentty/runtime/view/palette.hpp"

namespace agentty::ui {

using namespace maya;
using namespace maya::dsl;
// `login::` resolves to `agentty::ui::login::` from this scope without an
// alias — and MSVC rejects an alias whose name shadows the existing
// nested namespace, so don't write one.

namespace {

// Shared key-hint footer. Each sub-state passes the keys that are
// meaningful in its context so the user always sees a complete map.
// Wrapped so long footers reflow on narrow terminals.
Element key_hints(std::initializer_list<std::pair<std::string, std::string>> hints) {
    // Flatten into one TextElement with styled runs so the painter can
    // wrap across rows on a narrow terminal. The previous hstack approach
    // overflowed without ever wrapping.
    std::string content;
    std::vector<StyledRun> runs;
    auto push_run = [&](std::string_view s, Style sty) {
        if (s.empty()) return;
        runs.push_back(StyledRun{
            .byte_offset = content.size(),
            .byte_length = s.size(),
            .style       = sty,
        });
        content.append(s);
    };
    const Style key_sty = Style{}.with_fg(fg).with_bold();
    const Style sep_sty = fg_dim(muted);
    const Style val_sty = fg_dim(muted);
    bool first = true;
    for (const auto& [k, v] : hints) {
        if (!first) push_run("   \xc2\xb7   ", sep_sty);   //   ·
        first = false;
        push_run(k, key_sty);
        push_run(" ", sep_sty);
        push_run(v, val_sty);
    }
    return Element{TextElement{
        .content = std::move(content),
        .style   = Style{}.with_fg(fg),
        .wrap    = TextWrap::Wrap,
        .runs    = std::move(runs),
    }};
}

// Wrap-mode body text — long URLs / explanation lines reflow rather
// than overflowing the modal.
Element body_text(std::string_view content, Style sty) {
    return Element{TextElement{
        .content = std::string{content},
        .style   = sty,
        .wrap    = TextWrap::Wrap,
    }};
}

// Render an inline single-line text input with a block cursor. Mirrors
// the composer's input style. `secret` masks each byte as a bullet so
// keys/codes don't sit in plaintext on screen.
//
// The input row sits inside its own thin-bordered rect that spans the
// modal width — readable at a glance, and the focus border tells the
// user where to type without needing a separate label row above.
Element input_row(std::string_view value, int cursor, bool secret,
                  std::string_view placeholder)
{
    std::string display;
    if (secret) {
        // Bytes, not codepoints — a full UTF-8 mask is overkill since
        // OAuth codes and API keys are ASCII.
        display.assign(value.size(), '*');
    } else {
        display.assign(value);
    }

    auto prefix = text("\xE2\x80\xBA ", fg_bold(accent));   // ›

    if (display.empty()) {
        // Placeholder + cursor at home position.
        auto caret = text(" ", Style{}.with_bold().with_inverse());
        Element ph_el = !placeholder.empty()
            ? Element{text(std::string{placeholder}, fg_dim(muted))}
            : Element{text(" ", fg_of(fg))};
        return (h(prefix, caret, std::move(ph_el))
                | padding(0, 1)
                | border(BorderStyle::Round)
                | bcolor(accent)).build();
    }

    if (cursor < 0) cursor = 0;
    if (cursor > static_cast<int>(display.size())) cursor = static_cast<int>(display.size());
    std::string before = display.substr(0, cursor);
    std::string at_cursor = (cursor < static_cast<int>(display.size()))
        ? std::string{display[cursor]} : std::string{" "};
    std::string after = (cursor + 1 < static_cast<int>(display.size()))
        ? display.substr(cursor + 1) : std::string{};
    return (h(
        prefix,
        text(before, fg_of(fg)),
        text(at_cursor, Style{}.with_bold().with_inverse()),
        text(after, fg_of(fg))
    ) | padding(0, 1)
      | border(BorderStyle::Round)
      | bcolor(accent)).build();
}

// Bordered, wrap-mode "URL panel": the URL fills the modal width and
// reflows across rows on narrow terminals, so the user can read every
// character without horizontal scrolling. Background-styled so it
// reads as a dedicated artifact (the thing to copy) rather than as
// inline prose.
Element url_panel(std::string_view url) {
    auto url_text = Element{TextElement{
        .content = std::string{url},
        .style   = Style{}.with_fg(code_path),   // bright_cyan — "this is a path/identifier"
        .wrap    = TextWrap::Wrap,
    }};
    return (v(url_text)
            | padding(0, 1)
            | border(BorderStyle::Round)
            | bcolor(code_path)).build();
}

Element panel_picking(bool failed, std::string_view fail_msg) {
    std::vector<Element> rows;
    rows.push_back(text("Sign in to agentty", fg_bold(fg)));
    rows.push_back(body_text(
        "Bring your own model. Pick how you want to connect — you can "
        "change this any time from the command palette.",
        fg_dim(muted)));
    rows.push_back(text(""));
    if (failed) {
        // Wrap-mode danger toast so a long error message stays inside
        // the modal instead of overflowing into the chrome.
        rows.push_back(body_text(
            std::string{"\xE2\x9A\xA0 "} + std::string{fail_msg},
            fg_of(danger)));
        rows.push_back(text(""));
    }
    // Numbered items are flush-left to share the column with the header
    // / description / hint rows. The title sits next to the number badge;
    // the subtitle continuation indents by exactly the badge width ("1) "
    // = 3 chars) so it visually hangs under the title, not under the number.
    //
    // API key is presented FIRST: it's the unambiguous, provider-neutral
    // path (any Anthropic/OpenAI-family key), whereas subscription OAuth is
    // a third-party-client path some users would rather avoid. Leading with
    // the key keeps the un-controversial option one keystroke away.
    rows.push_back(h(text("1) ", fg_bold(highlight)),
                     text("Paste an API key", fg_bold(fg))).build());
    rows.push_back(h(text("   ", fg_of(fg)),
                     body_text("Anthropic sk-ant-…, or any provider's key",
                               fg_dim(muted))).build());
    rows.push_back(text(""));
    rows.push_back(h(text("2) ", fg_bold(highlight)),
                     text("OAuth via claude.ai", fg_bold(fg))).build());
    rows.push_back(h(text("   ", fg_of(fg)),
                     body_text("use your Claude Pro / Max subscription",
                               fg_dim(muted))).build());
    rows.push_back(text(""));
    // Other backends (OpenAI, Groq, OpenRouter, Ollama, …) authenticate via
    // an env var, not this modal. Point first-run users who don't have a
    // Claude account at the provider picker so they're never stuck here.
    rows.push_back(body_text(
        "Using OpenAI, Groq, OpenRouter or a local Ollama model instead? "
        "Press Esc, then Ctrl-P to pick it — you can paste its key right there.",
        fg_dim(muted)));
    rows.push_back(text(""));
    rows.push_back(key_hints({{"1/2", "choose"}, {"Esc", "close"}}));
    return v(std::move(rows)).build();
}

Element panel_oauth_code(const login::OAuthCode& s) {
    std::vector<Element> rows;
    rows.push_back(text("OAuth via claude.ai", fg_bold(fg)));
    rows.push_back(body_text(
        "Step 1 — open this URL and authorize agentty:",
        fg_dim(muted)));
    rows.push_back(text(""));
    rows.push_back(url_panel(s.authorize_url));
    rows.push_back(text(""));
    rows.push_back(body_text(
        "Step 2 — paste the callback code below:",
        fg_dim(muted)));
    rows.push_back(text(""));
    rows.push_back(input_row(s.code_input, s.cursor, /*secret=*/true,
                             /*placeholder=*/"paste the code from claude.ai"));
    rows.push_back(text(""));
    // Two hint rows: top for the URL-side affordances, bottom for the
    // form submit / cancel. The empty-code shortcuts are the most
    // helpful surface — the user lands here with an empty field, so
    // bare `c` / `o` give them the URL without any modifier dance.
    rows.push_back(key_hints({
        {"c",   "copy URL"},
        {"o",   "open browser"},
        {"^Y",  "copy"},
        {"^O",  "open"},
    }));
    rows.push_back(key_hints({
        {"Enter", "submit code"},
        {"Esc",   "cancel"},
    }));
    return v(std::move(rows)).build();
}

Element panel_oauth_exchanging() {
    std::vector<Element> rows;
    rows.push_back(text("Exchanging authorization code\xE2\x80\xA6",   // …
                        fg_bold(fg)));
    rows.push_back(text(""));
    rows.push_back(body_text(
        "Talking to platform.claude.com — this should take a second.",
        fg_dim(muted)));
    rows.push_back(text(""));
    rows.push_back(key_hints({{"Esc", "cancel"}}));
    return v(std::move(rows)).build();
}

Element panel_api_key(const login::ApiKeyInput& s) {
    std::vector<Element> rows;
    const bool anthropic = s.provider.empty();
    if (anthropic) {
        rows.push_back(text("Anthropic API key", fg_bold(fg)));
        rows.push_back(body_text(
            "Paste an sk-ant-… key. It will be saved to "
            "~/.config/agentty/credentials.json (0600).",
            fg_dim(muted)));
        rows.push_back(text(""));
        rows.push_back(input_row(s.key_input, s.cursor, /*secret=*/true,
                                 /*placeholder=*/"sk-ant-…"));
    } else {
        rows.push_back(text(s.provider_label + " API key", fg_bold(fg)));
        rows.push_back(body_text(
            "Paste your " + s.provider_label + " API key to switch to it. "
            "It's saved to ~/.config/agentty settings so you won't be "
            "asked again.",
            fg_dim(muted)));
        rows.push_back(text(""));
        rows.push_back(input_row(s.key_input, s.cursor, /*secret=*/true,
                                 /*placeholder=*/"paste API key…"));
    }
    rows.push_back(text(""));
    rows.push_back(key_hints({{"Enter", "submit"}, {"Esc", "cancel"}}));
    return v(std::move(rows)).build();
}

Element panel_custom_host(const login::CustomHostInput& s) {
    std::vector<Element> rows;
    rows.push_back(text("Custom OpenAI-compatible host", fg_bold(fg)));
    rows.push_back(body_text(
        "Enter a host or host:port for any server that speaks the OpenAI "
        "chat API \xe2\x80\x94 llama.cpp, vLLM, LM Studio, a proxy, or a "
        "remote box. A non-443 port uses plain HTTP (the local-server "
        "convention); a bare host uses HTTPS on 443.",
        fg_dim(muted)));
    rows.push_back(text(""));
    rows.push_back(input_row(s.host_input, s.cursor, /*secret=*/false,
                             /*placeholder=*/"localhost:8080"));
    rows.push_back(text(""));
    rows.push_back(body_text(
        "Examples:  localhost:8080  \xc2\xb7  127.0.0.1:1234  \xc2\xb7  "
        "inference.example.com",
        fg_dim(muted)));
    rows.push_back(text(""));
    rows.push_back(key_hints({{"Enter", "connect"}, {"Esc", "cancel"}}));
    return v(std::move(rows)).build();
}

} // namespace

Element login_modal(const Model& m) {
    if (!login::is_open(m.ui.login)) return nothing();

    Element body = std::visit([](const auto& s) -> Element {
        using T = std::decay_t<decltype(s)>;
        if constexpr (std::same_as<T, login::Closed>) {
            return nothing();
        } else if constexpr (std::same_as<T, login::Picking>) {
            return panel_picking(false, "");
        } else if constexpr (std::same_as<T, login::OAuthCode>) {
            return panel_oauth_code(s);
        } else if constexpr (std::same_as<T, login::OAuthExchanging>) {
            return panel_oauth_exchanging();
        } else if constexpr (std::same_as<T, login::ApiKeyInput>) {
            return panel_api_key(s);
        } else if constexpr (std::same_as<T, login::CustomHostInput>) {
            return panel_custom_host(s);
        } else if constexpr (std::same_as<T, login::Failed>) {
            return panel_picking(true, s.message);
        }
    }, m.ui.login);

    // Responsive sizing: `min_width` floors the modal at a readable
    // width on tiny terminals; the Overlay's default Stretch lets it
    // grow to fill all available columns on wider terminals (minus the
    // Overlay's 2-col edge padding). No max cap — every text node uses
    // TextWrap::Wrap, so URLs and prose reflow naturally to whatever
    // width the terminal gives us. Capping at 96 cols left ~50 cols of
    // empty terminal on a typical 150-col window.
    return vstack()
        .padding(1, 2)
        .min_width(Dimension::fixed(48))
        .border(BorderStyle::Round)
        .border_color(accent)
        .border_text(" Sign in to agentty ",
                     BorderTextPos::Top, BorderTextAlign::Center)
        (std::move(body));
}

} // namespace agentty::ui
