// 05_init.cpp — the five ways to initialise, and which to use.
//
// Build: make 05_init && ./05_init

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

struct Point { int x; int y; };

static void the_five_forms() {
    std::puts("-- five forms --");
    int a;           // 1. default: INDETERMINATE for a local int. never read it.
    int b = 5;       // 2. copy
    int c(6);        // 3. direct
    int d{7};        // 4. list (braced) -- prefer this
    int e = {8};     // 5. copy-list
    (void)a;
    std::printf("b=%d c=%d d=%d e=%d   (a is left unread on purpose)\n",
                b, c, d, e);
}

static void braces_catch_narrowing() {
    std::puts("\n-- braces reject narrowing --");
    std::uint8_t ok{255};
    std::printf("uint8_t ok{255}  = %u\n", static_cast<unsigned>(ok));
    std::puts("uint8_t bad{256};      -> COMPILE ERROR (narrowing)");
    std::puts("uint8_t bad = 256;     -> compiles, gives you 0");
    std::puts("int i{3.5};            -> COMPILE ERROR");
    std::puts("int i = 3.5;           -> compiles, gives you 3");
    std::puts("that difference is the whole argument for {}.");
}

static void the_vector_gotcha() {
    std::puts("\n-- the one place braces surprise you --");
    std::vector<int> paren(3, 7);    // three elements, each 7
    std::vector<int> brace{3, 7};    // two elements: 3 and 7

    std::printf("vector<int> v(3, 7) -> size %zu: ", paren.size());
    for (int v : paren) std::printf("%d ", v);
    std::putchar('\n');

    std::printf("vector<int> v{3, 7} -> size %zu: ", brace.size());
    for (int v : brace) std::printf("%d ", v);
    std::putchar('\n');
    std::puts("initializer_list always wins when one is viable.");
    std::puts("so: braces by default, parens when you mean a count.");
}

static void most_vexing_parse() {
    std::puts("\n-- the most vexing parse --");
    std::puts("std::string s();   declares a FUNCTION returning string.");
    std::puts("std::string s{};   declares a string. braces are unambiguous.");
    std::string s{};
    std::printf("s.empty() = %s\n", s.empty() ? "true" : "false");
}

static void aggregates_and_designators() {
    std::puts("\n-- aggregates --");
    Point p{3, 4};
    std::printf("Point p{3, 4}          -> x=%d y=%d\n", p.x, p.y);

    Point q{.x = 10, .y = 20};        // C++20 designated initialisers
    std::printf("Point q{.x=10, .y=20}  -> x=%d y=%d\n", q.x, q.y);
    std::puts("designators are how agentty writes LazyBytes::Source:");
    std::puts("  Source{.blob = std::move(name), .b64 = {}}");
    std::puts("order must match declaration order. you cannot skip around.");

    Point z{};                        // value-init: all members zeroed
    std::printf("Point z{}              -> x=%d y=%d  (zeroed)\n", z.x, z.y);
}

int main() {
    the_five_forms();
    braces_catch_narrowing();
    the_vector_gotcha();
    most_vexing_parse();
    aggregates_and_designators();
}
