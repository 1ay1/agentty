// web_search_form.cpp — Settings → Web Search row model.

#include "agentty/runtime/panel/web_search_form.hpp"

#include "agentty/runtime/settings_registry.hpp"

#include <mcp/tools/web_search.hpp>

#include <algorithm>
#include <string_view>
#include <vector>

#include <string>
#include <type_traits>
#include <variant>

namespace agentty::web_search_form {

namespace reg = settings::registry;

namespace {

constexpr std::string_view kKeyClearPrefix = "web_search.keyclear.";

bool is_slot_row(std::string_view id) {
    return id == "web_search.primary" || id == "web_search.secondary" || id == "web_search.fallback";
}

const std::string& slot_value(const web_search_cfg::Config& c, std::string_view id) {
    return id == "web_search.primary" ? c.primary
         : id == "web_search.secondary" ? c.secondary : c.fallback;
}

std::string dial_key(std::string_view service, std::string_view dial) {
    std::string k{service};
    k += '.';
    k += dial;
    return k;
}

std::string trim(std::string_view s) {
    constexpr std::string_view kSpace = " \t";
    const auto b = s.find_first_not_of(kSpace);
    if (b == std::string_view::npos) return {};
    return std::string{s.substr(b, s.find_last_not_of(kSpace) - b + 1)};
}

// Whether `value` may be stored for the text dial `key` ("<service>.<dial>"):
// mcp-cpp's own rule, so the pane never saves what a search would drop.
bool text_dial_ok(std::string_view key, std::string_view value) {
    const auto dot = key.find('.');
    if (dot == std::string_view::npos) return false;
    const auto* svc = ::mcp::tools::find_web_search_service(key.substr(0, dot));
    if (!svc) return false;
    for (const auto& d : svc->dials)
        if (d.id == key.substr(dot + 1)) return ::mcp::tools::web_search_dial_value_ok(d, value);
    return false;
}

// "Brave Free \xe2\x86\x92 DuckDuckGo Free \xe2\x86\x92 Tavily Free" for the subtitle.
std::string chain_text(const web_search_cfg::Config& c) {
    std::string out;
    for (const std::string* id : {&c.primary, &c.secondary, &c.fallback}) {
        const auto* svc = ::mcp::tools::find_web_search_service(*id);
        if (!svc) continue;
        if (!out.empty()) out += " \xe2\x86\x92 ";
        out += svc->label;
    }
    return out.empty() ? std::string{"Brave Free"} : out;
}

// The catalogue's labels on a slot row. add_row builds a Choice whose labels
// are the raw ids ("brave-free"); the ids stay what is stored, the labels
// become what a person reads.
void label_slot_choice(form::Field& row) {
    auto* ch = std::get_if<form::field::Choice>(&row.value);
    if (!ch) return;
    ch->ids = ch->labels;
    ch->hints.clear();
    for (auto& l : ch->labels) {
        if (l == "none") { l = "None"; ch->hints.emplace_back("leave this slot empty"); continue; }
        const auto* svc = ::mcp::tools::find_web_search_service(l);
        if (!svc) { ch->hints.emplace_back(); continue; }
        l = svc->label;
        ch->hints.emplace_back(svc->needs_key ? "needs an API key" : "free, no account");
    }
}

// The rows that hang under one slot: the service's dials, then its key.
void add_service_rows(form::Builder& b, const web_search_cfg::Config& c,
                      const ::mcp::tools::WebSearchService& svc,
                      const KeyOrigin& key_origin) {
    for (const auto& d : svc.dials) {
        std::string cur;
        if (auto it = c.dials.find(dial_key(svc.id, d.id)); it != c.dials.end())
            cur = it->second;
        std::string help = svc.label + (d.help.empty() ? "" : " \xc2\xb7 " + d.help);
        std::string row_id = std::string{kDialPrefix} + dial_key(svc.id, d.id);
        if (d.text != ::mcp::tools::WebSearchText::None) {
            b.text(std::move(row_id), "  " + d.label, cur, std::move(help));
        } else {
            std::vector<std::string> labels, ids;
            for (const auto& v : d.values) { labels.push_back(v.label); ids.push_back(v.id); }
            b.choice(std::move(row_id), "  " + d.label, std::move(labels), std::move(ids),
                     cur, std::move(help));
        }
        b.origin(cur.empty() ? "default" : "settings.json");
    }
    if (!svc.needs_key) return;

    const std::string origin = key_origin ? key_origin(svc.id) : std::string{};
    const bool stored   = origin == "stored";
    const bool from_env = origin.rfind("env: ", 0) == 0;
    const std::string env_var = from_env ? origin.substr(5) : std::string{};

    // The help line is what explains a locked row, so it names the way out.
    std::string help = svc.label + " \xc2\xb7 ";
    if (stored)
        help += "key saved, stored encrypted \xc2\xb7 Remove key below to replace it";
    else if (from_env)
        help += "key from " + env_var + " \xc2\xb7 unset it to store one here";
    else
        help += "paste a key and press Enter \xc2\xb7 stored encrypted, never in "
                "settings.json \xc2\xb7 " + svc.signup;
    b.secret(std::string{kKeyPrefix} + svc.id, "  API key", "", std::move(help));

    // What the row says is ONE phrase in the value column (the Secret's
    // placeholder), never repeated in the origin column. The wording and
    // glyphs are the Providers panel's: a key we hold reads
    // "\xe2\x9c\x93 key saved", one from the environment "\xe2\x97\x8f key from VAR"
    // (it is not saved, and there is no Remove row for it). No key: the
    // widget's own "not set". Before the first read answers, "checking...",
    // so the row claims neither way.
    if (stored)               b.placeholder("\xe2\x9c\x93 key saved");
    else if (from_env)        b.placeholder("\xe2\x97\x8f key from " + env_var);
    else if (!origin.empty()) b.placeholder(origin);
    // A held key locks its row: a stored one is changed by removing it first
    // (Remove key, below), and an env key is not ours to change at all. The
    // reason is left empty -- the value column already says it.
    if (stored || from_env) b.lock("");
    // Only a key we hold can be removed. One from the environment is not ours
    // to delete, and the row above is locked for the same reason.
    if (stored)
        b.action(std::string{kKeyClearPrefix} + svc.id, "  Remove key",
                 "forget the stored " + svc.label + " key", "press Enter");
}

} // namespace

std::string_view key_row_service(std::string_view row_id) noexcept {
    if (row_id.rfind(kKeyPrefix, 0) == 0) return row_id.substr(kKeyPrefix.size());
    return {};
}

std::string_view key_clear_service(std::string_view row_id) noexcept {
    if (row_id.rfind(kKeyClearPrefix, 0) == 0) return row_id.substr(kKeyClearPrefix.size());
    return {};
}

form::Form build_form(const web_search_cfg::Config& c, const reg::EnvSnapshot& env,
                      const KeyOrigin& key_origin) {
    form::Builder b{" Web Search "};
    // Say what the setting governs: the chain in force, by name.
    switch (web_search_cfg::mode(c)) {
        case web_search_cfg::Mode::On:
            b.subtitle("web_search \xc2\xb7 your counts apply \xc2\xb7 " + chain_text(c));
            break;
        case web_search_cfg::Mode::Off:
            b.subtitle("web_search is off \xc2\xb7 the agent cannot search the web");
            break;
        case web_search_cfg::Mode::Auto:
            b.subtitle("web_search \xc2\xb7 the model sets the count, as before \xc2\xb7 " + chain_text(c));
            break;
    }

    bool first = true;
    auto last_group = reg::Group::WebSearch;
    std::vector<std::string> shown;   // services whose rows are already out
    // Every search row is Basic: each one answers a question a user can
    // actually have ("why so few results?", "why did it not search?"). Walked
    // here rather than through add_rows only so a slot's dials and key can be
    // placed directly under it.
    for (const auto& d : reg::kSettings) {
        if (d.owner() != reg::Owner::WebSearch) continue;
        if (first || d.group != last_group) {
            b.header(std::string{reg::label_of(d.group)});
            last_group = d.group;
            first = false;
        }
        reg::add_row(b, c, env, d);
        if (!is_slot_row(d.id)) continue;
        const std::string& id = slot_value(c, d.id);
        const auto* svc = ::mcp::tools::find_web_search_service(id);
        if (!svc || std::find(shown.begin(), shown.end(), id) != shown.end()) continue;
        shown.push_back(id);
        add_service_rows(b, c, *svc, key_origin);
    }

    form::Form f = b.build();
    for (auto& row : f.fields)
        if (is_slot_row(row.id)) label_slot_choice(row);

    // Rows that do nothing in the current mode are locked, with a reason, and
    // still shown so the user can see what they hold. apply_form skips locked
    // rows, so their stored values survive a switch to a mode that uses them.
    //   auto: the model sets the count, so the two count rows are inert.
    //   off:  nothing is searched, so every row but the switch is inert.
    // An env lock is left alone: its reason (the variable) is the truer one.
    // A held key's lock has no reason of its own (its value says it), so it
    // takes the mode's.
    auto lock = [](form::Field& row, const char* why) {
        if (row.locked && !row.locked_reason.empty()) return;
        row.locked = true;
        row.locked_reason = why;
    };
    auto lock_id = [&](const char* id, const char* why) {
        if (auto* row = f.find(id)) lock(*row, why);
    };
    switch (web_search_cfg::mode(c)) {
        case web_search_cfg::Mode::Auto:
            for (const char* id : {"web_search.count", "web_search.max_count"})
                lock_id(id, "auto: the model sets the count \xe2\x80\x94 switch to on to set it");
            break;
        case web_search_cfg::Mode::Off:
            for (auto& row : f.fields) {
                if (row.id == "web_search.mode" || row.is_header()) continue;
                lock(row, "off: web search is off \xe2\x80\x94 switch to auto or on");
            }
            break;
        case web_search_cfg::Mode::On:
            break;
    }
    return f;
}


void apply_form(const form::Form& f, web_search_cfg::Config& c) {
    reg::read_rows(c, f, reg::Owner::WebSearch);
    // Dial rows. They are not registry rows (which dials exist depends on the
    // services chosen), so they are found by their id prefix. A dial set back
    // to the service's own default (or a text row emptied) is erased, not
    // stored as "". Typed text mcp-cpp would not send is not stored either:
    // the rebuild then shows the row with what was on record.
    for (const auto& row : f.fields) {
        if (row.locked || row.id.rfind(kDialPrefix, 0) != 0) continue;
        const std::string key = row.id.substr(kDialPrefix.size());
        std::string val;
        if (const auto* ch = std::get_if<form::field::Choice>(&row.value)) {
            val = std::string{ch->id()};
        } else if (const auto* tx = std::get_if<form::field::Text>(&row.value)) {
            val = trim(tx->value);
            if (!val.empty() && !text_dial_ok(key, val)) continue;
        } else {
            continue;
        }
        if (val.empty()) c.dials.erase(key);
        else             c.dials[key] = val;
    }
}

} // namespace agentty::web_search_form
