// web_search_form.cpp — Settings → Web Search row model.

#include "agentty/runtime/panel/web_search_form.hpp"

#include "agentty/runtime/settings_registry.hpp"

#include <string>
#include <type_traits>
#include <variant>

namespace agentty::web_search_form {

namespace reg = settings::registry;

form::Form build_form(const web_search_cfg::Config& c) {
    form::Builder b{" Web Search "};
    // Say what the setting governs and where it stops: the engine chain is
    // the tool's own (mcp-cpp), so naming it here is the honest answer to
    // "can I pick the engine?" without a row that cannot do anything.
    switch (web_search_cfg::mode(c)) {
        case web_search_cfg::Mode::On:
            b.subtitle("web_search \xc2\xb7 your counts apply \xc2\xb7 Brave \xe2\x86\x92 DuckDuckGo \xe2\x86\x92 Startpage");
            break;
        case web_search_cfg::Mode::Off:
            b.subtitle("web_search is off \xc2\xb7 the agent cannot search the web");
            break;
        case web_search_cfg::Mode::Auto:
            b.subtitle("web_search \xc2\xb7 the model sets the count, as before \xc2\xb7 Brave \xe2\x86\x92 DuckDuckGo \xe2\x86\x92 Startpage");
            break;
    }
    bool first = true;
    auto last_group = reg::Group::WebSearch;
    // Every search row is Basic: each one answers a question a user can
    // actually have ("why so few results?", "why did it not search?").
    reg::add_rows(b, c, reg::Owner::WebSearch, /*advanced=*/true, first, last_group);
    form::Form f = b.build();
    // Rows that do nothing in the current mode are locked, with a reason, and
    // still shown so the user can see what they hold. apply_form skips locked
    // rows, so their stored values survive a switch to a mode that uses them.
    //   auto: the model sets the count, so the two count rows are inert.
    //   off:  nothing is searched, so every row but the switch is inert.
    // An env lock is left alone: its reason (the variable) is the truer one.
    auto lock = [&f](const char* id, const char* why) {
        if (auto* row = f.find(id); row && !row->locked) {
            row->locked = true;
            row->locked_reason = why;
        }
    };
    switch (web_search_cfg::mode(c)) {
        case web_search_cfg::Mode::Auto:
            for (const char* id : {"web_search.count", "web_search.max_count"})
                lock(id, "auto: the model sets the count \xe2\x80\x94 switch to on to set it");
            break;
        case web_search_cfg::Mode::Off:
            for (const char* id : {"web_search.count", "web_search.max_count", "web_search.exclude_sites"})
                lock(id, "off: web search is off \xe2\x80\x94 switch to auto or on");
            break;
        case web_search_cfg::Mode::On:
            break;
    }
    return f;
}

namespace {

void write_row(const form::Field& row, const reg::SettingDef& d,
               web_search_cfg::Config& c) {
    std::visit([&](const auto& v) {
        using T = std::decay_t<decltype(v)>;
        if constexpr (std::is_same_v<T, form::field::Toggle>)
            (void)reg::set(c, d, v.on ? "true" : "false");
        else if constexpr (std::is_same_v<T, form::field::Number>)
            (void)reg::set(c, d, std::to_string(v.value));
        else if constexpr (std::is_same_v<T, form::field::Slider>)
            (void)reg::set(c, d, std::to_string(v.value));
        else if constexpr (std::is_same_v<T, form::field::Choice>)
            (void)reg::set(c, d, std::string{v.id()});
        else if constexpr (std::is_same_v<T, form::field::Text>)
            (void)reg::set(c, d, v.value);
    }, row.value);
}

} // namespace

void apply_form(const form::Form& f, web_search_cfg::Config& c) {
    // Walk the SAME rows build_form emitted, so a row cannot be written that
    // was never shown, and a row added to the table is picked up both ways.
    for (const auto& d : reg::kSettings) {
        if (d.owner() != reg::Owner::WebSearch) continue;
        const auto* row = f.find(d.id);
        if (!row || row->locked) continue;
        write_row(*row, d, c);
    }
}

} // namespace agentty::web_search_form
