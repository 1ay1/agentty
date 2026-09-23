// 07_lifetime.cpp — when objects die, and the four ways to outlive them.
//
// NOTE: the dangling examples are COMMENTED OUT on purpose. Uncomment one
// at a time and run under -fsanitize=address to watch it get caught.
//
// Build: make 07_lifetime && ./07_lifetime

#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

struct Loud {
    std::string tag;
    explicit Loud(std::string t) : tag(std::move(t)) {
        std::printf("  + %s\n", tag.c_str());
    }
    ~Loud() { std::printf("  - %s\n", tag.c_str()); }
};

// ── destruction order is reverse of construction ───────────────────────
static void destruction_order() {
    std::puts("-- scope: last in, first out --");
    Loud a{"a"};
    Loud b{"b"};
    {
        Loud inner{"inner"};
        std::puts("  (inner scope ending)");
    }
    Loud c{"c"};
    std::puts("  (function ending)");
}

// ── members die in reverse declaration order, after the body ───────────
struct Owner {
    Loud first{"member-first"};
    Loud second{"member-second"};
    ~Owner() { std::puts("  ~Owner body runs BEFORE members are destroyed"); }
};

static void member_order() {
    std::puts("\n-- members --");
    Owner o;
    std::puts("  (o going out of scope)");
}

// ── the four dangling shapes ───────────────────────────────────────────
static void dangling_catalogue() {
    std::puts("\n-- dangling, all four shapes (see source, they're commented) --");

    std::puts("1. reference to a local:");
    std::puts("     const std::string& f() { std::string s = \"x\"; return s; }");

    std::puts("2. view into a temporary:");
    std::puts("     std::string_view sv = std::string(\"temp\");");
    std::puts("   the string dies at the end of THAT statement. sv is garbage.");

    std::puts("3. iterator/reference invalidated by growth:");
    std::vector<int> v{1, 2, 3};
    int* p = &v[0];
    std::printf("     &v[0] before push_back = %p\n", static_cast<void*>(p));
    v.reserve(1000);                    // may reallocate
    std::printf("     &v[0] after  reserve   = %p\n", static_cast<void*>(&v[0]));
    std::puts("   if those differ, every saved pointer just went stale.");

    std::puts("4. pointer to a member of a moved-from object:");
    std::puts("     the buffer moved away; the pointer still aims at the old one.");
}

// ── string_view: the sharpest of the four ──────────────────────────────
static std::string build() { return "built on the fly"; }

static void string_view_rules() {
    std::puts("\n-- string_view is a borrow, always --");

    std::string owned = "i own my bytes";
    std::string_view ok = owned;              // fine: owned outlives ok
    std::printf("ok  = '%.*s'\n", static_cast<int>(ok.size()), ok.data());

    // std::string_view bad = build();        // DANGLING. temp dies here.
    std::puts("std::string_view bad = build();   <- dangles immediately");

    std::string keep = build();               // keep it alive first
    std::string_view good = keep;
    std::printf("good = '%.*s'\n", static_cast<int>(good.size()), good.data());

    std::puts("rule: a string_view parameter is fine. a string_view MEMBER");
    std::puts("or return value needs you to prove the owner outlives it.");
}

// ── static and thread_local ────────────────────────────────────────────
static int& counter() {
    static int n = 0;      // constructed on first call, destroyed at exit
    return ++n;
}

static void storage_durations() {
    std::puts("\n-- storage durations --");
    std::puts("automatic  : locals, die at end of scope");

    // NOTE: printing all three in ONE printf would be a trap. argument
    // evaluation order is UNSPECIFIED, so you might see 3, 2, 1. sequence
    // them with separate statements when the calls have side effects.
    std::printf("static     : counter() -> ");
    std::printf("%d, ", counter());
    std::printf("%d, ", counter());
    std::printf("%d (survives calls)\n", counter());

    std::puts("dynamic    : new/delete, or better, a smart pointer (ch05)");
    std::puts("thread     : thread_local, one per thread");
}

int main() {
    destruction_order();
    member_order();
    dangling_catalogue();
    string_view_rules();
    storage_durations();
}
