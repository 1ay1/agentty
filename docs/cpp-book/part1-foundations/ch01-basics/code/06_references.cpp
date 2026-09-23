// 06_references.cpp — references, const, and how to pass parameters.
//
// Build: make 06_references && ./06_references

#include <cstdio>
#include <string>

struct Noisy {
    std::string name;
    explicit Noisy(std::string n) : name(std::move(n)) {
        std::printf("  ctor  %s\n", name.c_str());
    }
    Noisy(const Noisy& o) : name(o.name) {
        std::printf("  COPY  %s\n", name.c_str());
    }
    Noisy(Noisy&& o) noexcept : name(std::move(o.name)) {
        std::printf("  move  %s\n", name.c_str());
    }
    ~Noisy() { std::printf("  dtor  %s\n", name.empty() ? "(moved-from)" : name.c_str()); }
};

// ── a reference is another name for the same object ────────────────────
static void aliasing() {
    std::puts("-- a reference is an alias, not a pointer --");
    int  x = 1;
    int& r = x;            // r IS x
    r = 99;
    std::printf("x = %d after writing through r\n", x);
    std::printf("&x == &r ? %s\n", (&x == &r) ? "yes" : "no");
    std::puts("you cannot rebind r. `r = y` writes y's value INTO x.");
    std::puts("a reference has no null state and must be initialised.");
}

// ── const reference: read-only view, no copy ───────────────────────────
static void by_value(Noisy n)        { std::printf("  in by_value:  %s\n", n.name.c_str()); }
static void by_cref(const Noisy& n)  { std::printf("  in by_cref:   %s\n", n.name.c_str()); }
static void by_ref(Noisy& n)         { n.name += "!"; }

static void parameter_passing() {
    std::puts("\n-- passing --");
    Noisy n{"payload"};

    std::puts(" by_value(n):");
    by_value(n);                    // copies in, destroys at end of call

    std::puts(" by_cref(n):");
    by_cref(n);                     // nothing printed = nothing copied

    std::puts(" by_ref(n) then read:");
    by_ref(n);
    std::printf("  n.name = %s\n", n.name.c_str());

    std::puts(" end of scope:");
}

// ── const binds to temporaries and extends their life ──────────────────
static std::string make() { return "temporary"; }

static void const_ref_lifetime() {
    std::puts("\n-- const& extends a temporary --");
    const std::string& r = make();   // the temporary lives as long as r
    std::printf("r = '%s'   still alive\n", r.c_str());
    std::puts("this ONLY works when the const& binds the temporary directly.");
    std::puts("const std::string& bad = make().substr(0,4);  <- also fine");
    std::puts("but a reference to a MEMBER of a temporary is not extended.");
}

// ── const is about the view, not the object ────────────────────────────
static void const_views() {
    std::puts("\n-- const is a property of the ACCESS PATH --");
    std::string s = "mutable object";
    const std::string& v = s;        // read-only view of a mutable object
    std::printf("through v: '%s'\n", v.c_str());
    s += " changed underneath";      // legal: s itself is not const
    std::printf("through v: '%s'\n", v.c_str());
    std::puts("v promised not to write. it never promised nobody else would.");
}

// ── mutable: the LazyBytes pattern ─────────────────────────────────────
// This is agentty's include/agentty/domain/lazy_bytes.hpp in miniature.
class Payload {
public:
    explicit Payload(std::string blob_name) : blob_(std::move(blob_name)) {}

    // const, because materialising is not a LOGICAL mutation. the caller
    // asked for bytes; where they came from is our business.
    [[nodiscard]] const std::string& bytes() const {
        if (!resolved_) {
            std::printf("  (resolving blob '%s' ... )\n", blob_.c_str());
            bytes_    = "decoded:" + blob_;
            resolved_ = true;
        }
        return bytes_;
    }

private:
    std::string         blob_;
    mutable std::string bytes_;          // mutable = writable through const
    mutable bool        resolved_ = false;
};

static void mutable_cache() {
    std::puts("\n-- mutable, and why it is honest here --");
    const Payload p{"img-7f3a"};         // note: const
    std::printf("first  bytes(): %s\n", p.bytes().c_str());
    std::printf("second bytes(): %s\n", p.bytes().c_str());
    std::puts("second call did not resolve. same answer both times, so from");
    std::puts("the caller's side nothing changed. that is what makes the");
    std::puts("mutable legitimate: bitwise change, logical constness.");
}

int main() {
    aliasing();
    parameter_passing();
    const_ref_lifetime();
    const_views();
    mutable_cache();
}
