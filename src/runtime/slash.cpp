// slash.cpp — the one catalog + resolver for `/name`. See slash.hpp.
#include "agentty/runtime/slash.hpp"

#include "agentty/runtime/fuzzy.hpp"
#include "agentty/runtime/model.hpp"

#include <algorithm>
#include <climits>
#include <unordered_set>

namespace agentty::slash {

namespace {

[[nodiscard]] bool is_space(char c) noexcept {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

Entry action(std::string name, std::string desc, Action a, std::string hint = {}) {
    Entry e;
    e.name        = std::move(name);
    e.description = std::move(desc);
    e.hint        = std::move(hint);
    e.kind        = Kind::Action;
    e.action      = a;
    e.takes_args  = !e.hint.empty();
    return e;
}

}  // namespace

const std::vector<Entry>& builtin_actions() {
    static const std::vector<Entry> v = {
        action("new",      "Start a new thread",                    Action::New),
        action("compact",  "Summarise the thread to free up context", Action::Compact),
        action("model",    "Switch model",                          Action::Model),
        action("rename",   "Rename this thread",                    Action::Rename, "<title>"),
        action("fork",     "Fork into a fresh thread",              Action::Fork),
        action("rewind",   "Rewind to a checkpoint",                Action::Rewind),
        action("review",   "Review pending changes",                Action::Review),
        action("threads",  "Open the thread list",                  Action::Threads),
        action("settings", "Open settings",                         Action::Settings),
        action("login",    "Sign in or add an account",             Action::Login),
        action("quit",     "Quit agentty",                          Action::Quit),
    };
    return v;
}

Catalog make_catalog(const std::vector<Source>& commands,
                     const std::vector<Source>& skills,
                     const std::vector<Source>& prompts,
                     bool with_actions) {
    Catalog c;
    std::unordered_set<std::string> seen;
    auto take = [&](const Entry& e) {
        if (e.name.empty() || !seen.insert(e.name).second) return;
        c.entries.push_back(e);
    };
    if (with_actions)
        for (const auto& e : builtin_actions()) take(e);
    auto add = [&](const std::vector<Source>& src, Kind k) {
        for (const auto& s : src) {
            Entry e;
            e.name        = s.name;
            e.description = s.description;
            e.hint        = s.hint;
            e.kind        = k;
            take(e);
        }
    };
    add(commands, Kind::Command);
    add(skills,   Kind::Skill);
    add(prompts,  Kind::Prompt);
    return c;
}

std::optional<Head> head(std::string_view text) {
    if (text.empty() || text.front() != '/') return std::nullopt;
    std::size_t end = 1;
    while (end < text.size() && !is_space(text[end])) ++end;
    Head h;
    h.name = text.substr(1, end - 1);
    if (end < text.size()) {
        h.has_space = true;
        std::size_t a = end;
        while (a < text.size() && (text[a] == ' ' || text[a] == '\t')) ++a;
        h.args = text.substr(a);
    }
    return h;
}

Resolved resolve(std::string_view text, const Catalog& cat) {
    Resolved r;
    const auto h = head(text);
    if (!h || h->name.empty()) return r;
    const auto it = std::ranges::find(cat.entries, h->name, &Entry::name);
    if (it == cat.entries.end()) return r;   // /etc/hosts, /unknown: plain text
    r.entry = &*it;
    r.args  = std::string{h->args};
    switch (it->kind) {
        case Kind::Action:  r.what = Resolved::What::Action;  break;
        case Kind::Command: r.what = Resolved::What::Command; break;
        case Kind::Skill:   r.what = Resolved::What::Skill;   break;
        case Kind::Prompt:  r.what = Resolved::What::Prompt;  break;
    }
    return r;
}

std::optional<MenuQuery> menu_query(std::string_view text, int cursor) {
    if (text.empty() || text.front() != '/') return std::nullopt;
    if (cursor < 1 || static_cast<std::size_t>(cursor) > text.size()) return std::nullopt;
    // The token runs from the '/' to the first whitespace.
    std::size_t end = 1;
    while (end < text.size() && !is_space(text[end])) ++end;
    if (static_cast<std::size_t>(cursor) > end) return std::nullopt;   // past the name
    const std::string_view name = text.substr(1, end - 1);
    if (name.find('/') != std::string_view::npos) return std::nullopt;   // a path
    // Attachment placeholders start with a control byte; a token holding
    // one isn't a name being typed.
    for (char c : name)
        if (static_cast<unsigned char>(c) < 0x20) return std::nullopt;
    return MenuQuery{name};
}

std::vector<Match> filter(const Catalog& cat, std::string_view query) {
    std::vector<Match> out;
    out.reserve(cat.entries.size());
    if (query.empty()) {
        for (const auto& e : cat.entries) out.push_back(Match{&e, 0, {}});
        return out;
    }
    auto lower = [](std::string_view s) {
        std::string o{s};
        for (char& c : o)
            if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        return o;
    };
    const std::string q = lower(query);
    for (std::size_t i = 0; i < cat.entries.size(); ++i) {
        const Entry& e = cat.entries[i];
        const std::string n = lower(e.name);
        Match m{&e, INT_MIN, {}};
        if (n == q) {
            m.score = 1'000'000;
        } else if (n.starts_with(q)) {
            m.score = 500'000 - static_cast<int>(n.size());   // shorter first
        } else if (auto f = fuzzy::score(e.name, query); f.matched()) {
            m.score = 100'000 + f.score;
            m.hits  = std::move(f.positions);
        } else if (lower(e.description).find(q) != std::string::npos) {
            m.score = 1'000;
        }
        if (m.score == INT_MIN) continue;
        if (m.hits.empty() && m.score >= 500'000)
            for (std::size_t k = 0; k < q.size(); ++k) m.hits.push_back(static_cast<int>(k));
        // Ties keep catalog order (actions first), so subtract the index.
        m.score -= static_cast<int>(i);
        out.push_back(std::move(m));
    }
    std::ranges::stable_sort(out, [](const Match& a, const Match& b) { return a.score > b.score; });
    return out;
}

Completed complete(std::string_view text, const Entry& entry) {
    std::size_t end = 1;
    while (end < text.size() && !is_space(text[end])) ++end;
    Completed c;
    c.text = "/" + entry.name + " ";
    std::string_view rest = end < text.size() ? text.substr(end) : std::string_view{};
    // Don't stack a second space when one already follows the token.
    if (!rest.empty() && rest.front() == ' ') rest.remove_prefix(1);
    c.cursor = static_cast<int>(c.text.size());
    c.text += rest;
    return c;
}

Catalog catalog_of(const Model& m) {
    std::vector<Source> cmds, skills, prompts;
    for (const auto& c : m.ui.library.commands)
        cmds.push_back({c.name, c.description, c.argument_hint});
    for (const auto& s : m.ui.library.skills)
        if (!s.name.empty()) skills.push_back({s.name, s.description, {}});
    for (const auto& p : m.ui.plugins.prompts)
        prompts.push_back({p.name, p.description, p.hint});
    return make_catalog(cmds, skills, prompts);
}

std::vector<Match> menu_rows(const Model& m, const Catalog& cat) {
    const auto& cs = m.ui.composer;
    const auto q = menu_query(cs.text, cs.cursor);
    if (!q) return {};
    if (cs.slash.dismissed && *cs.slash.dismissed == q->query) return {};
    return filter(cat, q->query);
}

}  // namespace agentty::slash
