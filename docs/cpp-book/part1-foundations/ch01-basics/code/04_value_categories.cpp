// 04_value_categories.cpp — lvalue, prvalue, xvalue, and what std::move is.
//
// Build: make 04_value_categories && ./04_value_categories

#include <cstdio>
#include <string>
#include <utility>

// Two overloads. Which one the compiler picks tells you the value
// category of the argument expression. This is the only reliable way to
// SEE a value category, so we use it everywhere below.
static void cat(int&)  { std::puts("lvalue"); }
static void cat(int&&) { std::puts("rvalue"); }

// ── the categories ─────────────────────────────────────────────────────
static int  global = 100;
static int& give_lvalue() { return global; }   // returns a reference
static int  give_prvalue() { return 42; }      // returns a value

static void categories() {
    std::puts("-- what kind of expression is this? --");
    int x = 1;
    int arr[3]{};

    std::printf("x                 -> "); cat(x);
    std::printf("42                -> "); cat(42);
    std::printf("x + 1             -> "); cat(x + 1);
    std::printf("arr[0]            -> "); cat(arr[0]);
    std::printf("give_lvalue()     -> "); cat(give_lvalue());
    std::printf("give_prvalue()    -> "); cat(give_prvalue());
    std::printf("std::move(x)      -> "); cat(std::move(x));
    std::puts("\nrule of thumb: can you take its address and will it still");
    std::puts("be there next line? then it is an lvalue.");
}

// ── the trap everybody hits: a named rvalue reference is an lvalue ─────
static void inside(int&& r) {
    std::printf("  param declared int&&, but `r` itself   -> "); cat(r);
    std::printf("  std::move(r)                           -> "); cat(std::move(r));
}

static void named_rvalue_ref() {
    std::puts("\n-- a named rvalue reference is an LVALUE --");
    inside(42);
    std::puts("  this is why you still write std::move(x) when forwarding");
    std::puts("  an int&& parameter onward. the name makes it an lvalue again.");
}

// ── std::move is a cast. it moves nothing. ─────────────────────────────
static void move_is_a_cast() {
    std::puts("\n-- std::move does not move --");
    std::string a = "payload";

    // INTENTIONAL: std::move is [[nodiscard]]; discarding it is precisely
    // the mistake this demo exists to show. The (void) keeps the build
    // clean while the lesson stays visible.
    (void)std::move(a);
    std::printf("after a bare std::move(a):  a = '%s'   <- untouched\n",
                a.c_str());

    std::string b = std::move(a);   // NOW the move constructor runs
    std::printf("after b = std::move(a):     a = '%s'  b = '%s'\n",
                a.c_str(), b.c_str());
    std::puts("std::move only changes the expression's category. the actual");
    std::puts("stealing is done by whatever constructor or assignment runs.");
}

// ── moved-from is valid but unspecified ────────────────────────────────
static void moved_from_state() {
    std::puts("\n-- what is left behind --");
    std::string src = "a fairly long string to defeat SSO buffers";
    std::string dst = std::move(src);
    std::printf("dst        = '%s'\n", dst.c_str());
    std::printf("src.size() = %zu   (valid object, unspecified value)\n",
                src.size());
    src = "reassigned";            // always legal
    std::printf("src        = '%s'   <- assigning is always fine\n",
                src.c_str());
    std::puts("you may destroy or assign a moved-from object. do not READ it.");
}

int main() {
    categories();
    named_rvalue_ref();
    move_is_a_cast();
    moved_from_state();
}
