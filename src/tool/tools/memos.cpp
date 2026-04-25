// `remember` / `forget` / `memos` — explicit hooks into the persistent
// workspace memory the agent accumulates.
//
// `investigate` writes memos automatically; these tools let the model
// (or a curious user via the wire) curate that memory directly:
//
//   remember(topic, content, files?)   — append a fact the agent
//        wants to keep handy across turns. Useful when knowledge is
//        learned via a normal read/grep/edit loop, not a dedicated
//        investigate run.
//
//   forget(id_or_query)                — drop a memo that's stale,
//        wrong, or outdated.
//
//   memos()                            — list every memo with its id,
//        topic, age, and which files it references. The model uses
//        this to discover what's already known before deciding to
//        investigate / read / grep again.

#include "moha/auth/auth.hpp"
#include "moha/index/repo_index.hpp"
#include "moha/memory/memo_store.hpp"
#include "moha/provider/anthropic/transport.hpp"
#include "moha/runtime/app/deps.hpp"
#include "moha/runtime/msg.hpp"
#include "moha/tool/spec.hpp"
#include "moha/tool/tools.hpp"
#include "moha/tool/util/arg_reader.hpp"
#include "moha/tool/util/fs_helpers.hpp"
#include "moha/tool/util/subprocess.hpp"
#include "moha/tool/util/tool_args.hpp"
#include "moha/tool/util/utf8.hpp"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <sstream>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include <nlohmann/json.hpp>

namespace moha::tools {

using json = nlohmann::json;

namespace {

// ── remember ───────────────────────────────────────────────────────
struct RememberArgs {
    std::string topic;
    std::string content;
    std::vector<std::string> files;
    std::string display_description;
};

[[nodiscard]] std::expected<RememberArgs, ToolError>
parse_remember_args(const json& j) {
    util::ArgReader ar(j);
    auto t = ar.require_str("topic");
    if (!t) return std::unexpected(ToolError::invalid_args(
        "topic required: a one-line description of what this memo is about"));
    auto c = ar.require_str("content");
    if (!c) return std::unexpected(ToolError::invalid_args(
        "content required: the body of the fact to remember"));
    std::vector<std::string> files;
    if (const json* fs = ar.raw("files"); fs && fs->is_array()) {
        for (const auto& v : *fs) {
            if (v.is_string()) files.push_back(v.get<std::string>());
        }
    }
    return RememberArgs{
        *std::move(t), *std::move(c),
        std::move(files),
        ar.str("display_description", ""),
    };
}

ExecResult run_remember(const RememberArgs& a) {
    auto& store = memory::shared();
    if (!store.ready())
        return std::unexpected(ToolError::io(
            "memo store not bound to a workspace"));
    memory::Memo m;
    m.query     = a.topic;
    m.synthesis = a.content;
    m.created_at = std::chrono::system_clock::now();
    m.file_refs  = a.files;
    m.source     = "manual";
    m.base_score = 80;     // explicit save = high trust
    store.add(std::move(m));
    std::ostringstream out;
    out << "\xe2\x9c\x93 remembered: \"" << a.topic << "\"\n"
        << "  " << a.content.size() << " chars";
    if (!a.files.empty()) {
        out << "  ·  " << a.files.size() << " file ref"
            << (a.files.size() == 1 ? "" : "s");
    }
    out << "  ·  " << store.size() << " memo"
        << (store.size() == 1 ? "" : "s") << " total\n";
    out << "This memo will be in the system prompt for every future turn "
           "in this workspace until forgotten.";
    std::string body = out.str();
    if (!a.display_description.empty())
        body = a.display_description + "\n\n" + body;
    return ToolOutput{std::move(body), std::nullopt};
}

ToolDef tool_remember_impl() {
    ToolDef t;
    constexpr const auto& kSpec = spec::require<"remember">();
    t.name = ToolName{std::string{kSpec.name}};
    t.description =
        "Save a fact about this codebase to long-term memory. Persisted "
        "to <workspace>/.moha/memos.json and INJECTED INTO THE SYSTEM "
        "PROMPT for every subsequent turn (cached on Anthropic's side, "
        "so the cost amortises after the first turn). Use when you've "
        "learned something durable that you'd otherwise have to "
        "rediscover next session — architecture decisions, where things "
        "live, gotchas, contracts between modules. NOT for transient "
        "in-conversation context. Format the `content` as concise "
        "markdown; you can include code spans, paths, line numbers.";
    t.input_schema = json{
        {"type","object"},
        {"required", {"topic","content"}},
        {"properties", {
            {"display_description", {{"type","string"},
                {"description","One-line summary shown in the UI. Optional."}}},
            {"topic", {{"type","string"},
                {"description","One-line topic — the question this memo answers, e.g. \"how does the OAuth refresh flow work?\"."}}},
            {"content", {{"type","string"},
                {"description","Markdown body. Names, line numbers, key relationships, gotchas."}}},
            {"files", {{"type","array"}, {"items", {{"type","string"}}},
                {"description","Optional: workspace-relative paths whose mtime governs whether this memo is still fresh."}}},
        }},
    };
    t.effects = kSpec.effects;
    t.eager_input_streaming = kSpec.eager_input_streaming;
    t.execute = util::adapt<RememberArgs>(parse_remember_args, run_remember);
    return t;
}

// ── forget ─────────────────────────────────────────────────────────
struct ForgetArgs {
    std::string target;     // id or substring of query
    std::string display_description;
};

[[nodiscard]] std::expected<ForgetArgs, ToolError>
parse_forget_args(const json& j) {
    util::ArgReader ar(j);
    auto t = ar.require_str("target");
    if (!t) return std::unexpected(ToolError::invalid_args(
        "target required: a memo id (from `memos()`) or a substring of its topic"));
    return ForgetArgs{
        *std::move(t),
        ar.str("display_description", ""),
    };
}

ExecResult run_forget(const ForgetArgs& a) {
    // Walk the memo list, find matches, remove. Since MemoStore doesn't
    // expose a remove API, do the work inline by clearing + re-adding.
    // Simpler: extend MemoStore with a remove() method.
    auto& store = memory::shared();
    if (!store.ready())
        return std::unexpected(ToolError::io(
            "memo store not bound to a workspace"));
    std::size_t removed = store.forget(a.target);
    std::ostringstream out;
    if (removed == 0) {
        out << "no memo matched '" << a.target
            << "' — call `memos()` to see what's stored";
    } else {
        out << "\xe2\x9c\x97 forgot " << removed << " memo"
            << (removed == 1 ? "" : "s") << " matching '"
            << a.target << "'  ·  " << store.size() << " remaining";
    }
    std::string body = out.str();
    if (!a.display_description.empty())
        body = a.display_description + "\n\n" + body;
    return ToolOutput{std::move(body), std::nullopt};
}

ToolDef tool_forget_impl() {
    ToolDef t;
    constexpr const auto& kSpec = spec::require<"forget">();
    t.name = ToolName{std::string{kSpec.name}};
    t.description =
        "Remove a memo from the workspace memory. Use when a memo is "
        "stale, wrong, or contradicts something you've just learned. "
        "Pass either the memo's id (from `memos()`) or a substring of "
        "its topic.";
    t.input_schema = json{
        {"type","object"},
        {"required", {"target"}},
        {"properties", {
            {"display_description", {{"type","string"},
                {"description","One-line summary shown in the UI. Optional."}}},
            {"target", {{"type","string"},
                {"description","Memo id or topic substring (case-insensitive)."}}},
        }},
    };
    t.effects = kSpec.effects;
    t.eager_input_streaming = kSpec.eager_input_streaming;
    t.execute = util::adapt<ForgetArgs>(parse_forget_args, run_forget);
    return t;
}

// ── memos ──────────────────────────────────────────────────────────
struct MemosArgs {
    std::string filter;     // substring filter on topic
    std::string display_description;
};

[[nodiscard]] std::expected<MemosArgs, ToolError>
parse_memos_args(const json& j) {
    util::ArgReader ar(j);
    return MemosArgs{
        ar.str("filter", ""),
        ar.str("display_description", ""),
    };
}

ExecResult run_memos(const MemosArgs& a) {
    auto& store = memory::shared();
    if (!store.ready())
        return std::unexpected(ToolError::io(
            "memo store not bound to a workspace"));
    auto all = store.list_memos();
    if (all.empty())
        return ToolOutput{
            "No memos yet for " + store.workspace().generic_string()
            + ". The first `investigate` (or explicit `remember`) "
            "will start populating workspace memory.",
            std::nullopt};

    std::string lower_filter = a.filter;
    std::ranges::transform(lower_filter, lower_filter.begin(),
        [](unsigned char c){ return static_cast<char>(std::tolower(c)); });

    // Sort by recency, descending.
    std::ranges::sort(all, [](const memory::Memo& x, const memory::Memo& y) {
        return x.created_at > y.created_at;
    });

    std::ostringstream out;
    auto now = std::chrono::system_clock::now();
    out << "Workspace memory: " << store.workspace().generic_string() << "\n";
    int shown = 0;
    for (const auto& m : all) {
        if (!lower_filter.empty()) {
            std::string lq = m.query;
            std::ranges::transform(lq, lq.begin(),
                [](unsigned char c){ return static_cast<char>(std::tolower(c)); });
            if (lq.find(lower_filter) == std::string::npos) continue;
        }
        ++shown;
        auto age_s = std::chrono::duration_cast<std::chrono::seconds>(
            now - m.created_at).count();
        std::string age;
        if      (age_s < 60)         age = std::to_string(age_s) + "s";
        else if (age_s < 3600)       age = std::to_string(age_s / 60) + "m";
        else if (age_s < 86400)      age = std::to_string(age_s / 3600) + "h";
        else                         age = std::to_string(age_s / 86400) + "d";
        bool fresh = store.is_fresh(m);
        out << "\n" << (fresh ? "\xe2\x9c\x93" : "\xe2\x9a\xa0") << " "
            << m.id << "  ·  " << age << " ago  ·  "
            << m.synthesis.size() << " chars";
        if (!m.file_refs.empty())
            out << "  ·  " << m.file_refs.size() << " file ref"
                << (m.file_refs.size() == 1 ? "" : "s");
        if (!fresh) out << "  ·  STALE (files changed)";
        out << "\n   Q: " << m.query << "\n";
        if (!m.file_refs.empty()) {
            out << "   files: ";
            std::size_t budget = 80;
            std::size_t i = 0;
            for (const auto& f : m.file_refs) {
                if (i > 0) out << ", ";
                if (f.size() + 2 > budget) {
                    out << "(+" << (m.file_refs.size() - i) << " more)";
                    break;
                }
                out << f;
                budget -= f.size() + 2;
                ++i;
            }
            out << "\n";
        }
    }
    if (shown == 0)
        out << "\n(no memos match filter '" << a.filter << "')";
    else
        out << "\n" << shown << "/" << all.size() << " shown.  "
            << "Call `forget(id)` to drop one.";
    std::string body = out.str();
    if (!a.display_description.empty())
        body = a.display_description + "\n\n" + body;
    return ToolOutput{std::move(body), std::nullopt};
}

ToolDef tool_memos_impl() {
    ToolDef t;
    constexpr const auto& kSpec = spec::require<"memos">();
    t.name = ToolName{std::string{kSpec.name}};
    t.description =
        "List the workspace memos — the persistent knowledge accumulated "
        "from past `investigate` runs and `remember` calls. Use BEFORE "
        "starting a fresh investigation: if a memo already covers the "
        "question, prefer answering from it (or from the "
        "<learned-about-this-workspace> system-prompt block, which "
        "carries the same content). Each memo shows its id, age, byte "
        "size, and file refs. Stale memos (files changed since the "
        "memo was written) get a ⚠ marker.";
    t.input_schema = json{
        {"type","object"},
        {"properties", {
            {"display_description", {{"type","string"},
                {"description","One-line summary shown in the UI. Optional."}}},
            {"filter", {{"type","string"},
                {"description","Optional substring filter on memo topics (case-insensitive)."}}},
        }},
    };
    t.effects = kSpec.effects;
    t.eager_input_streaming = kSpec.eager_input_streaming;
    t.execute = util::adapt<MemosArgs>(parse_memos_args, run_memos);
    return t;
}

// ── recall ────────────────────────────────────────────────────────
// Direct lookup that bypasses the system-prompt budget truncation.
// When the model sees a memo summary stub like "[stale memo — call
// `recall(...)`]" or just wants the unabridged text of a memo it
// caught the title of, it calls recall(topic) and gets the full
// synthesis verbatim.
struct RecallArgs {
    std::string topic;
    std::string display_description;
};

[[nodiscard]] std::expected<RecallArgs, ToolError>
parse_recall_args(const json& j) {
    util::ArgReader ar(j);
    auto t = ar.require_str("topic");
    if (!t) return std::unexpected(ToolError::invalid_args(
        "topic required: a memo id, exact topic, or substring"));
    return RecallArgs{*std::move(t), ar.str("display_description", "")};
}

ExecResult run_recall(const RecallArgs& a) {
    auto& store = memory::shared();
    if (!store.ready())
        return std::unexpected(ToolError::io(
            "memo store not bound to a workspace"));
    // Try id-exact first, then substring search via find_similar.
    auto by_id = store.by_id(a.topic);
    if (by_id) {
        std::ostringstream out;
        out << "# " << by_id->query << "\n"
            << "(memo " << by_id->id << " · "
            << by_id->effective_confidence(store.workspace())
            << "% confidence)\n\n"
            << by_id->synthesis;
        std::string body = out.str();
        if (!a.display_description.empty())
            body = a.display_description + "\n\n" + body;
        return ToolOutput{std::move(body), std::nullopt};
    }
    auto match = store.find_similar(a.topic);
    if (!match)
        return ToolOutput{
            "No memo matches '" + a.topic + "'. Call `memos()` to "
            "list everything stored for this workspace, or `investigate("
            + a.topic + ")` to learn it now.", std::nullopt};
    std::ostringstream out;
    out << "# " << match->query << "\n"
        << "(memo " << match->id << " · "
        << match->effective_confidence(store.workspace())
        << "% confidence)\n\n"
        << match->synthesis;
    std::string body = out.str();
    if (!a.display_description.empty())
        body = a.display_description + "\n\n" + body;
    return ToolOutput{std::move(body), std::nullopt};
}

ToolDef tool_recall_impl() {
    ToolDef t;
    constexpr const auto& kSpec = spec::require<"recall">();
    t.name = ToolName{std::string{kSpec.name}};
    t.description =
        "Fetch the FULL text of a stored memo by id or topic substring. "
        "The system prompt's <learned-about-this-workspace> block is "
        "byte-budgeted and may show only summary stubs for older or "
        "lower-confidence memos. Use `recall` whenever you've spotted "
        "a relevant topic in the prompt and want the unabridged "
        "synthesis before answering.";
    t.input_schema = json{
        {"type","object"},
        {"required", {"topic"}},
        {"properties", {
            {"display_description", {{"type","string"},
                {"description","One-line summary shown in the UI. Optional."}}},
            {"topic", {{"type","string"},
                {"description","Memo id (from `memos()`) or any substring of its topic."}}},
        }},
    };
    t.effects = kSpec.effects;
    t.eager_input_streaming = kSpec.eager_input_streaming;
    t.execute = util::adapt<RecallArgs>(parse_recall_args, run_recall);
    return t;
}

// ── find_usages ───────────────────────────────────────────────────
// Cross-file reference graph query. Built into RepoIndex during the
// importance-scoring tokenisation pass — answers "who uses Foo?"
// in zero tool sub-calls (no grep needed).
struct FindUsagesArgs {
    std::string symbol;
    std::string display_description;
};

[[nodiscard]] std::expected<FindUsagesArgs, ToolError>
parse_find_usages_args(const json& j) {
    util::ArgReader ar(j);
    auto s = ar.require_str("symbol");
    if (!s) return std::unexpected(ToolError::invalid_args(
        "symbol required: the identifier to find usages of"));
    return FindUsagesArgs{*std::move(s), ar.str("display_description", "")};
}

ExecResult run_find_usages(const FindUsagesArgs& a) {
    auto& idx = index::shared();
    if (!idx.ready())
        idx.refresh(std::filesystem::current_path());
    auto users = idx.files_using(a.symbol);
    int count = idx.reference_count(a.symbol);
    if (users.empty())
        return ToolOutput{
            "Symbol '" + a.symbol + "' has no recorded cross-file "
            "uses in the workspace index. It might be defined but "
            "unused, defined in a non-indexed file, or you may need "
            "to refresh the index via `repo_map`.", std::nullopt};
    std::ostringstream out;
    out << "Symbol '" << a.symbol << "' is referenced in "
        << count << " other file" << (count == 1 ? "" : "s") << ":\n";
    auto root = idx.workspace();
    int shown = 0;
    constexpr int kMax = 50;
    for (const auto& p : users) {
        if (shown >= kMax) {
            out << "  ... +" << (users.size() - shown) << " more\n";
            break;
        }
        std::error_code ec;
        auto rel = std::filesystem::relative(p, root, ec);
        out << "  " << (ec ? p.generic_string() : rel.generic_string())
            << "\n";
        ++shown;
    }
    out << "\n(Use `outline(path)` for any of these to see how the "
           "symbol is used, or `read(path, …)` for the source.)";
    std::string body = out.str();
    if (!a.display_description.empty())
        body = a.display_description + "\n\n" + body;
    return ToolOutput{std::move(body), std::nullopt};
}

ToolDef tool_find_usages_impl() {
    ToolDef t;
    constexpr const auto& kSpec = spec::require<"find_usages">();
    t.name = ToolName{std::string{kSpec.name}};
    t.description =
        "List the files that REFERENCE a given symbol (function, class, "
        "type, etc.). Backed by the workspace's symbol index — answers "
        "in microseconds, no grep / sub-agent needed. Use BEFORE editing "
        "a public-looking definition to know its blast radius, or to "
        "trace where some constant / pattern is used. Pair with "
        "`find_definition` (which finds where it's *defined*) for the "
        "complete picture. If the index doesn't know the symbol, fall "
        "back to `grep` or `signatures`.";
    t.input_schema = json{
        {"type","object"},
        {"required", {"symbol"}},
        {"properties", {
            {"display_description", {{"type","string"},
                {"description","One-line summary shown in the UI. Optional."}}},
            {"symbol", {{"type","string"},
                {"description","Identifier to find references of. Case-sensitive, exact match."}}},
        }},
    };
    t.effects = kSpec.effects;
    t.eager_input_streaming = kSpec.eager_input_streaming;
    t.execute = util::adapt<FindUsagesArgs>(parse_find_usages_args, run_find_usages);
    return t;
}

// ── mine_adrs ─────────────────────────────────────────────────────
// Walks the git log and asks Haiku to extract architectural decisions
// from commit messages. Each detected decision becomes an ADR memo
// (source = "adr") that lives in the system prompt forever — the
// agent inherits the project's design history without anyone having
// to re-document it.
struct MineAdrsArgs {
    std::string since;       // "30 days ago", "1 month", etc.
    int max_commits;         // hard cap on commits scanned
    std::string display_description;
};

[[nodiscard]] std::expected<MineAdrsArgs, ToolError>
parse_mine_adrs_args(const json& j) {
    util::ArgReader ar(j);
    int n = ar.integer("max_commits", 200);
    if (n < 1)    n = 1;
    if (n > 1000) n = 1000;
    return MineAdrsArgs{
        ar.str("since", "180 days ago"),
        n,
        ar.str("display_description", ""),
    };
}

// Synchronous Haiku call asking "is this commit message documenting
// an architectural decision? if yes, distill <decision> + <rationale>".
// Returns empty when the model replies "no" or the call fails.
[[nodiscard]] std::string
distill_adr_via_haiku(const std::string& subject,
                      const std::string& body) {
    const auto& d = app::deps();
    if (d.auth_header.empty()) return "";
    constexpr const char* kAdrPrompt =
        "You triage git commit messages for architectural decisions. "
        "An architectural decision is a CHOICE made between alternatives "
        "with a stated rationale (\"switched from X to Y because Z\", "
        "\"removed feature W because V\", \"adopted pattern P\"). "
        "Respond in EXACTLY ONE of two formats:\n"
        "  SKIP\n"
        "      — when the commit is just a fix, refactor, dependency "
        "        bump, or anything without a decision rationale.\n"
        "  DECISION: <one-line decision>\n"
        "  WHY: <one-line rationale>\n"
        "      — when the commit IS documenting a decision.\n"
        "Output ONLY one of those two — no other text, no preamble.";
    Message instr;
    instr.role = Role::User;
    instr.text = "Subject: " + subject + "\n\n"
               + (body.empty() ? std::string{"(no body)"} : body);
    provider::anthropic::Request req;
    req.model         = "claude-haiku-4-5-20251001";
    req.system_prompt = kAdrPrompt;
    req.messages      = std::vector<Message>{std::move(instr)};
    req.tools         = {};
    req.max_tokens    = 200;
    req.auth_header   = d.auth_header;
    req.auth_style    = d.auth_style;
    std::string reply;
    bool finished = false;
    try {
        provider::anthropic::run_stream_sync(std::move(req), [&](Msg m) {
            std::visit([&](auto&& e) {
                using T = std::decay_t<decltype(e)>;
                if constexpr (std::same_as<T, StreamTextDelta>) {
                    reply += e.text;
                } else if constexpr (std::same_as<T, StreamFinished>
                                  || std::same_as<T, StreamError>) {
                    finished = true;
                }
            }, m);
        }, /*cancel*/{});
    } catch (...) { return ""; }
    if (!finished) return "";
    while (!reply.empty() && (reply.front() == ' ' || reply.front() == '\n'))
        reply.erase(0, 1);
    if (reply.starts_with("SKIP")) return "";
    return reply;
}

ExecResult run_mine_adrs(const MineAdrsArgs& a) {
    auto& store = memory::shared();
    if (!store.ready())
        return std::unexpected(ToolError::io(
            "memo store not bound to a workspace"));
    auto root = store.workspace();
    std::error_code ec;
    if (!std::filesystem::exists(root / ".git", ec))
        return std::unexpected(ToolError::not_found(
            "workspace is not a git repo — nothing to mine"));

    // Pull commits with subject + body via a delimiter format.
    auto r = util::run_argv_s(
        std::vector<std::string>{
            "git", "-C", root.string(), "log",
            "--since=" + a.since,
            "-n", std::to_string(a.max_commits),
            "--pretty=format:\x01%H%n%s%n%b%x02"
        },
        /*max_bytes=*/4 * 1024 * 1024,
        std::chrono::seconds{20});
    if (!r.started || r.exit_code != 0)
        return std::unexpected(ToolError::subprocess(
            "git log failed: " + r.output.substr(0, 256)));

    // Split on \x02 — each block is "\x01<sha>\n<subject>\n<body>".
    std::vector<std::tuple<std::string, std::string, std::string>> commits;
    std::size_t pos = 0;
    while (pos < r.output.size()) {
        auto end = r.output.find('\x02', pos);
        std::string blk = r.output.substr(pos,
            end == std::string::npos ? r.output.size() - pos : end - pos);
        pos = end == std::string::npos ? r.output.size() : end + 1;
        auto soh = blk.find('\x01');
        if (soh == std::string::npos) continue;
        blk = blk.substr(soh + 1);
        auto nl1 = blk.find('\n');
        if (nl1 == std::string::npos) continue;
        std::string sha = blk.substr(0, nl1);
        auto rest = blk.substr(nl1 + 1);
        auto nl2 = rest.find('\n');
        std::string subj = rest.substr(0, nl2);
        std::string body = nl2 == std::string::npos ? "" : rest.substr(nl2 + 1);
        // Filter: subject + body > 80 chars suggests a thoughtful commit
        // worth checking. Pure-fix / dependency-bump commits are short.
        if (subj.size() + body.size() < 80) continue;
        commits.emplace_back(std::move(sha), std::move(subj), std::move(body));
    }

    if (commits.empty())
        return ToolOutput{
            "No commits in the window had message bodies long enough "
            "to suggest a documented decision (≥80 chars). Nothing "
            "mined.", std::nullopt};

    int distilled = 0;
    int kept      = 0;
    int skipped   = 0;
    std::ostringstream rep;
    for (const auto& [sha, subj, body] : commits) {
        // Dedupe — if a memo already exists for this commit, skip.
        std::string topic = "ADR: " + subj;
        if (store.by_id(sha)) { ++skipped; continue; }
        bool already_exists = false;
        for (const auto& existing : store.list_memos()) {
            if (existing.source == "adr" && existing.query == topic) {
                already_exists = true; break;
            }
        }
        if (already_exists) { ++skipped; continue; }

        auto reply = distill_adr_via_haiku(subj, body);
        ++distilled;
        if (reply.empty()) continue;     // SKIP

        memory::Memo m;
        m.id         = sha.substr(0, 16);     // first 16 hex of full SHA
        m.query      = topic;
        m.synthesis  = reply;
        m.created_at = std::chrono::system_clock::now();
        m.git_head   = sha;
        m.source     = "adr";
        m.model      = "claude-haiku-4-5-20251001";
        m.base_score = 75;
        store.add(std::move(m));
        ++kept;
    }

    rep << "Mined " << commits.size() << " candidate commit"
        << (commits.size() == 1 ? "" : "s") << " from the last "
        << a.since << ":\n"
        << "  · " << kept << " stored as ADR memo"
        << (kept == 1 ? "" : "s") << "\n"
        << "  · " << (distilled - kept) << " skipped (no decision)\n"
        << "  · " << skipped << " already in memory\n\n"
        << "Run `memos()` to inspect; the new ADRs are now in the "
        << "system prompt for every future turn.";
    std::string body = rep.str();
    if (!a.display_description.empty())
        body = a.display_description + "\n\n" + body;
    return ToolOutput{std::move(body), std::nullopt};
}

ToolDef tool_mine_adrs_impl() {
    ToolDef t;
    constexpr const auto& kSpec = spec::require<"mine_adrs">();
    t.name = ToolName{std::string{kSpec.name}};
    t.description =
        "Mine the git log for architectural decisions and persist them "
        "as ADR memos. For each commit with a message body ≥80 chars, "
        "asks Haiku to detect whether it documents a decision (\"X "
        "switched to Y because Z\", \"removed W because V\") and stores "
        "the distilled DECISION+WHY pair in workspace memory. The ADRs "
        "then appear in the system prompt for every future turn so the "
        "agent inherits the project's design history. Idempotent — "
        "skips commits already mined. Use ONCE per workspace to seed; "
        "re-run periodically to pick up new decisions.";
    t.input_schema = json{
        {"type","object"},
        {"properties", {
            {"display_description", {{"type","string"},
                {"description","One-line summary shown in the UI. Optional."}}},
            {"since", {{"type","string"},
                {"description","Git --since= window. Default '180 days ago'. Use git's natural-language time grammar."}}},
            {"max_commits", {{"type","integer"},
                {"description","Hard cap on commits scanned (default 200, max 1000)."}}},
        }},
    };
    t.effects = kSpec.effects;
    t.eager_input_streaming = kSpec.eager_input_streaming;
    t.execute = util::adapt<MineAdrsArgs>(parse_mine_adrs_args, run_mine_adrs);
    return t;
}

} // namespace

ToolDef tool_remember()    { return tool_remember_impl(); }
ToolDef tool_forget()      { return tool_forget_impl(); }
ToolDef tool_memos()       { return tool_memos_impl(); }
ToolDef tool_recall()      { return tool_recall_impl(); }
ToolDef tool_find_usages() { return tool_find_usages_impl(); }
ToolDef tool_mine_adrs()   { return tool_mine_adrs_impl(); }

} // namespace moha::tools
