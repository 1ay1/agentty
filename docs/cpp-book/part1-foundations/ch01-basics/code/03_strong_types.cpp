// 03_strong_types.cpp — agentty's Id<Tag>, built up from nothing.
//
// This is a faithful rebuild of include/agentty/domain/id.hpp, minus the
// json hooks (which need nlohmann). Read that header after this file and
// you will recognise every line.
//
// Build: make 03_strong_types && ./03_strong_types

#include <compare>
#include <cstdio>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// ── the problem: two ids that are both "some hex" ──────────────────────
namespace weak {
struct Thread { std::string id; };
// both parameters are std::string. swap them at the call site and it
// still compiles, still runs, and quietly does the wrong thing.
inline void cancel(const std::string& thread_id, const std::string& call_id) {
    std::printf("  cancel(thread=%s, call=%s)\n",
                thread_id.c_str(), call_id.c_str());
}
} // namespace weak

// ── the fix: a phantom tag makes two structurally identical types ──────
template <typename Tag>
struct Id {
    std::string value;

    Id() = default;
    explicit Id(std::string s) noexcept : value(std::move(s)) {}

    [[nodiscard]] bool        empty() const noexcept { return value.empty(); }
    [[nodiscard]] const char* c_str() const noexcept { return value.c_str(); }

    bool operator==(const Id&) const = default;
    auto operator<=>(const Id&) const = default;

    [[nodiscard]] bool operator==(std::string_view sv) const noexcept {
        return value == sv;
    }
};

// Tag types. They are never instantiated. They exist only so that
// Id<ThreadIdTag> and Id<ToolCallIdTag> are different types.
struct ThreadIdTag   {};
struct ToolCallIdTag {};
struct ModelIdTag    {};

using ThreadId   = Id<ThreadIdTag>;
using ToolCallId = Id<ToolCallIdTag>;
using ModelId    = Id<ModelIdTag>;

static void cancel(const ThreadId& t, const ToolCallId& c) {
    std::printf("  cancel(thread=%s, call=%s)\n", t.c_str(), c.c_str());
}

// ── it costs nothing ───────────────────────────────────────────────────
static void zero_overhead() {
    std::puts("-- the wrapper is free --");
    std::printf("sizeof(std::string) = %zu\n", sizeof(std::string));
    std::printf("sizeof(ThreadId)    = %zu   <- identical\n", sizeof(ThreadId));
    std::printf("sizeof(ModelId)     = %zu\n", sizeof(ModelId));
    std::puts("a phantom tag adds no member, so it adds no byte.");
}

// ── what each keyword buys you ─────────────────────────────────────────
static void explicit_matters() {
    std::puts("\n-- explicit --");
    ThreadId t{"abc123"};
    std::printf("ThreadId t{\"abc123\"}  -> %s\n", t.c_str());
    std::puts("ThreadId t = \"abc123\"   is a COMPILE ERROR.");
    std::puts("without explicit, any string would silently become a ThreadId");
    std::puts("and the whole point of the type would be gone.");
}

static void nodiscard_matters() {
    std::puts("\n-- [[nodiscard]] --");
    ThreadId t{"x"};
    (void)t.empty();
    std::puts("a bare `t.empty();` warns. empty() asks a question and");
    std::puts("throwing away the answer is always a bug.");
}

static void comparison() {
    std::puts("\n-- defaulted == and <=> --");
    ThreadId a{"aaa"}, b{"bbb"}, a2{"aaa"};
    std::printf("a == a2 ? %s\n", (a == a2) ? "yes" : "no");
    std::printf("a <  b  ? %s   <- <=> gives you <, >, <=, >= for free\n",
                (a < b) ? "yes" : "no");
    std::printf("a == \"aaa\" (string_view overload) ? %s\n",
                (a == "aaa") ? "yes" : "no");

    // and because <=> exists, it sorts.
    std::vector<ThreadId> v{ThreadId{"c"}, ThreadId{"a"}, ThreadId{"b"}};
    std::printf("sortable: ");
    for (std::size_t i = 0; i < v.size(); ++i)
        for (std::size_t j = i + 1; j < v.size(); ++j)
            if (v[j] < v[i]) std::swap(v[i], v[j]);
    for (const auto& id : v) std::printf("%s ", id.c_str());
    std::putchar('\n');
}

static void the_bug_it_catches() {
    std::puts("\n-- the bug --");
    std::puts("weak version, arguments swapped, compiles fine:");
    std::string tid = "thread-1", cid = "call-9";
    weak::cancel(cid, tid);          // swapped. no warning. wrong behaviour.

    std::puts("strong version, correct order:");
    ThreadId t{"thread-1"};
    ToolCallId c{"call-9"};
    cancel(t, c);
    std::puts("strong version swapped -> cancel(c, t) is a COMPILE ERROR.");
}

int main() {
    zero_overhead();
    explicit_matters();
    nodiscard_matters();
    comparison();
    the_bug_it_catches();
}
