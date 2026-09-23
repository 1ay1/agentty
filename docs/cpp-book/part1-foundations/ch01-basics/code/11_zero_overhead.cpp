// 11_zero_overhead.cpp — prove the strong type costs nothing.
//
// This file exists to be COMPILED TO ASSEMBLY, not just run. Do both:
//
//   make 11_zero_overhead && ./11_zero_overhead     # the timing side
//   make proof                                       # the assembly side
//
// `make proof` compiles this at -O2 -S and diffs the weak and strong
// versions of each function. If the diff is empty, the abstraction is
// free. Not "approximately free". Free.

#include <chrono>
#include <cstdio>
#include <string>
#include <vector>

// ── the two versions ───────────────────────────────────────────────────
template <typename Tag>
struct Id {
    std::string value;
    Id() = default;
    explicit Id(std::string s) noexcept : value(std::move(s)) {}
    [[nodiscard]] bool empty() const noexcept { return value.empty(); }
    bool operator==(const Id&) const = default;
};
struct ThreadIdTag {};
using ThreadId = Id<ThreadIdTag>;

// Kept out of line and non-static so they survive to the .s file with
// stable mangled names that `make proof` can find.
std::size_t weak_len(const std::string& s)   { return s.size(); }
std::size_t strong_len(const ThreadId& id)   { return id.value.size(); }

bool weak_empty(const std::string& s)        { return s.empty(); }
bool strong_empty(const ThreadId& id)        { return id.empty(); }

std::size_t weak_total(const std::string* a, std::size_t n) {
    std::size_t t = 0;
    for (std::size_t i = 0; i < n; ++i) t += a[i].size();
    return t;
}
std::size_t strong_total(const ThreadId* a, std::size_t n) {
    std::size_t t = 0;
    for (std::size_t i = 0; i < n; ++i) t += a[i].value.size();
    return t;
}

// ── the timing side ────────────────────────────────────────────────────
template <typename F>
static double time_ms(F&& f) {
    const auto t0 = std::chrono::steady_clock::now();
    f();
    const auto t1 = std::chrono::steady_clock::now();
    return std::chrono::duration<double, std::milli>(t1 - t0).count();
}

int main() {
    constexpr std::size_t N = 2'000'000;

    std::vector<std::string> weak;
    std::vector<ThreadId>    strong;
    weak.reserve(N);
    strong.reserve(N);
    for (std::size_t i = 0; i < N; ++i) {
        std::string s = "thread-" + std::to_string(i);
        strong.emplace_back(s);          // copy
        weak.push_back(std::move(s));    // then move the original in
    }

    std::printf("-- sizes --\n");
    std::printf("sizeof(std::string)       = %zu\n", sizeof(std::string));
    std::printf("sizeof(ThreadId)          = %zu\n", sizeof(ThreadId));
    std::printf("sizeof(vector<string>)    = %zu\n", sizeof(std::vector<std::string>));
    std::printf("sizeof(vector<ThreadId>)  = %zu\n", sizeof(std::vector<ThreadId>));
    std::printf("%zu strings on the heap, both ways, byte for byte.\n\n", N);

    std::size_t sink = 0;
    const double tw = time_ms([&] { sink += weak_total(weak.data(), weak.size()); });
    const double ts = time_ms([&] { sink += strong_total(strong.data(), strong.size()); });

    std::printf("-- summing %zu lengths --\n", N);
    std::printf("weak   (std::string) : %7.2f ms\n", tw);
    std::printf("strong (ThreadId)    : %7.2f ms\n", ts);
    std::printf("(checksum %zu, ignore it, it just stops the optimiser\n", sink);
    std::printf(" from deleting the loops entirely)\n\n");

    std::puts("timings on a laptop are noisy. the ASSEMBLY is the real proof:");
    std::puts("  make proof");
    std::puts("if that prints 'identical' for every function, you are done");
    std::puts("arguing about whether the wrapper costs anything.");
}
