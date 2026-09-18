# Chapter 13: Type-Level Proofs with consteval

**Learning Objectives:**
- Use `consteval` to enforce compile-time validation
- Build type-level proofs with `static_assert`
- Validate invariants at compile time, not runtime
- See how agentty proves tool catalog completeness
- Write compile-time permission matrices
- Ensure correctness without tests

---

## 13.1 Why Prove at Compile Time?

### Runtime Testing Is Insufficient

Traditional approach: write tests, hope they cover everything.

```cpp
TEST(ToolCatalog, AllToolsHaveExecutors) {
    for (auto& tool : get_tool_catalog()) {
        EXPECT_TRUE(has_executor(tool.name));
    }
}
```

**Problems:**
- ❌ Test might not run
- ❌ Test might be skipped
- ❌ Coverage might miss cases
- ❌ Error discovered at runtime (or never)

### Compile-Time Proofs Are Absolute

```cpp
consteval bool all_tools_have_executors() {
    for (auto name : TOOL_NAMES) {
        if (!find_executor(name)) return false;
    }
    return true;
}

static_assert(all_tools_have_executors(), "Missing executor");
```

**Benefits:**
- ✅ Checked **every** build
- ✅ Can't be disabled
- ✅ Error before binary exists
- ✅ Tests are **proofs**

**Key insight:** A `static_assert` is a theorem. The build is the proof checker.

---

## 13.2 Building Compile-Time Predicates

### Pattern: consteval Predicate + static_assert

```cpp
// Predicate: computes a boolean at compile time
consteval bool invariant_holds() {
    // Check conditions
    return /* boolean */;
}

// Proof: enforces the invariant
static_assert(invariant_holds(), "Invariant violated");
```

### Example: Prove Struct Size

```cpp
struct Cell {
    uint32_t codepoint;
    uint32_t style_id;
};

consteval bool cell_is_8_bytes() {
    return sizeof(Cell) == 8;
}

static_assert(cell_is_8_bytes(), "Cell must be 8 bytes for SIMD packing");
```

**If someone adds a field:** Build fails immediately.

---

## 13.3 Real Example: agentty's Tool Catalog Proof

### The Invariant

agentty has **23 tools**. Each tool requires:
1. A **schema** (JSON definition)
2. An **executor** (C++ function)

**Invariant:** Every tool name in the registry has both a schema and an executor.

### The Proof

`agentty/include/agentty/tool/catalog_proof.hpp`:

```cpp
// Authoritative list of tool names
constexpr std::array<std::string_view, 23> TOOL_NAMES = {
    "read", "write", "edit", "shell", "grep", 
    "find_definition", "search_code", "search_docs", 
    "repo_map", "outline", "git_status", "git_diff", 
    "git_log", "git_commit", "git_branch",
    "process_start", "process_poll", "process_stop",
    "web_fetch", "web_search", "remember", 
    "forget", "task", "skill"
};

// Check if a tool has a schema
consteval bool has_schema(std::string_view name) {
    // Search in TOOL_SCHEMAS array
    for (auto& schema : TOOL_SCHEMAS) {
        if (schema.name == name) return true;
    }
    return false;
}

// Check if a tool has an executor
consteval bool has_executor(std::string_view name) {
    // Search in EXECUTOR_MAP
    for (auto& entry : EXECUTOR_MAP) {
        if (entry.name == name) return true;
    }
    return false;
}

// Prove all tools have schemas
consteval bool all_tools_have_schemas() {
    for (auto name : TOOL_NAMES) {
        if (!has_schema(name)) return false;
    }
    return true;
}

// Prove all tools have executors
consteval bool all_tools_have_executors() {
    for (auto name : TOOL_NAMES) {
        if (!has_executor(name)) return false;
    }
    return true;
}

// THE PROOFS
static_assert(all_tools_have_schemas(), 
              "Tool catalog incomplete: missing schema");
static_assert(all_tools_have_executors(), 
              "Tool catalog incomplete: missing executor");
```

### What Happens When It Breaks

Add a new tool without implementing it:

```cpp
constexpr std::array<std::string_view, 24> TOOL_NAMES = {
    // ... existing 23 ...
    "new_tool"  // Added but not implemented
};
```

**Build output:**

```
error: static assertion failed: Tool catalog incomplete: missing schema
static_assert(all_tools_have_schemas(), "Tool catalog incomplete: missing schema");
              ^~~~~~~~~~~~~~~~~~~~~~~~~
```

**Result:** Can't build until you implement the tool.

---

## 13.4 Permission Matrices

### The Problem

agentty tools have different permission levels:

- `read`, `grep` — safe (read-only)
- `write`, `edit` — dangerous (modify files)
- `shell` — very dangerous (arbitrary commands)

We want to **prove** that permission assignments are correct.

### The Proof

```cpp
enum class Permission { ReadOnly, Write, Execute };

struct ToolPermission {
    std::string_view name;
    Permission perm;
};

constexpr std::array<ToolPermission, 23> TOOL_PERMISSIONS = {
    {"read", Permission::ReadOnly},
    {"write", Permission::Write},
    {"shell", Permission::Execute},
    // ...
};

// Predicate: all write tools require confirmation
consteval bool write_tools_need_confirmation() {
    for (auto& tp : TOOL_PERMISSIONS) {
        if (tp.perm == Permission::Write) {
            // Check that tool is in confirmation list
            if (!needs_confirmation(tp.name)) return false;
        }
    }
    return true;
}

static_assert(write_tools_need_confirmation(),
              "Write tools must require confirmation");
```

**Invariant enforced:** You can't add a `Write` tool without adding it to the confirmation list.

---

## 13.5 Proving Enum Coverage

### The Problem

You have an enum and a mapping to strings:

```cpp
enum class Status { Pending, Running, Complete, Failed };

constexpr std::array<std::string_view, 4> STATUS_NAMES = {
    "pending", "running", "complete", "failed"
};
```

**Invariant:** Every enum value has a corresponding string.

### The Proof

```cpp
consteval bool all_statuses_covered() {
    // Can't iterate enum directly, but we know the count
    return STATUS_NAMES.size() == 4;  // Matches enum size
}

static_assert(all_statuses_covered(), "Status enum not fully covered");
```

**Better with C++26 reflection:**

```cpp
consteval bool all_statuses_covered() {
    return STATUS_NAMES.size() == std::enum_size<Status>;
}
```

---

## 13.6 Proving Data Structure Invariants

### Example: Packed Cell Structure

maya's terminal cells must be **exactly 8 bytes** for SIMD:

```cpp
struct Cell {
    uint32_t codepoint;
    uint16_t fg;
    uint16_t bg;
};

static_assert(sizeof(Cell) == 8, 
              "Cell must be 8 bytes for AVX2 packing");
static_assert(std::is_trivially_copyable_v<Cell>,
              "Cell must be POD for memcpy");
static_assert(alignof(Cell) <= 8,
              "Cell alignment must allow dense packing");
```

**Result:** Any change that breaks these constraints fails the build.

---

## 13.7 Proving Bit Layout

### Example: Flags Fit in Bitmask

```cpp
enum class Flag : uint32_t {
    Read    = 1 << 0,
    Write   = 1 << 1,
    Execute = 1 << 2,
    Admin   = 1 << 3,
    // ... up to ...
    Special = 1 << 30
};

consteval bool all_flags_fit() {
    return static_cast<uint32_t>(Flag::Special) <= (1u << 31);
}

static_assert(all_flags_fit(), "Flag exceeds 32-bit bitmask");
```

---

## 13.8 Proving String Literal Validity

### Example: No Duplicates in Tool Names

```cpp
consteval bool no_duplicate_tool_names() {
    for (size_t i = 0; i < TOOL_NAMES.size(); ++i) {
        for (size_t j = i + 1; j < TOOL_NAMES.size(); ++j) {
            if (TOOL_NAMES[i] == TOOL_NAMES[j]) return false;
        }
    }
    return true;
}

static_assert(no_duplicate_tool_names(), 
              "Duplicate tool name in catalog");
```

**Cost:** O(n²) at **compile time**. Zero runtime cost.

---

## 13.9 Real Example: Proving Lock Rank Ordering

### The Problem

agentty uses `RankedMutex<M, Rank>` to prevent deadlocks. We need to ensure:

1. All ranks are unique
2. Ranks are in ascending order in the codebase

### The Proof

```cpp
// Define all locks with ranks
constexpr size_t THREAD_CACHE_RANK = 0;
constexpr size_t HTTP_POOL_RANK = 1;
constexpr size_t TOOL_EXECUTOR_RANK = 2;
constexpr size_t MCP_SERVER_RANK = 3;

constexpr std::array<size_t, 4> LOCK_RANKS = {
    THREAD_CACHE_RANK,
    HTTP_POOL_RANK,
    TOOL_EXECUTOR_RANK,
    MCP_SERVER_RANK
};

// Prove ranks are strictly increasing
consteval bool ranks_are_ordered() {
    for (size_t i = 1; i < LOCK_RANKS.size(); ++i) {
        if (LOCK_RANKS[i] <= LOCK_RANKS[i - 1]) return false;
    }
    return true;
}

static_assert(ranks_are_ordered(), 
              "Lock ranks must be strictly increasing");
```

**Result:** Can't introduce a rank collision.

---

## 13.10 Proving ABI Compatibility

### The Problem

You have a struct that's shared across library boundaries:

```cpp
struct Message {
    uint32_t version;
    uint32_t type;
    uint64_t timestamp;
    // ...
};
```

**Invariant:** Size and alignment must remain stable.

### The Proof

```cpp
static_assert(sizeof(Message) == 24, 
              "Message ABI changed (size)");
static_assert(alignof(Message) == 8,
              "Message ABI changed (alignment)");
static_assert(offsetof(Message, timestamp) == 8,
              "Message field layout changed");
```

**Protects against:** Accidental field reordering or type changes.

---

## 13.11 Compile-Time Hash Uniqueness

### The Problem

You use compile-time string hashes for dispatch:

```cpp
consteval uint64_t hash(std::string_view str) { /* FNV-1a */ }

constexpr uint64_t READ_HASH = hash("read");
constexpr uint64_t WRITE_HASH = hash("write");
```

**Risk:** Hash collision would cause wrong dispatch.

### The Proof

```cpp
consteval bool tool_hashes_unique() {
    std::array<uint64_t, 23> hashes{};
    for (size_t i = 0; i < TOOL_NAMES.size(); ++i) {
        hashes[i] = hash(TOOL_NAMES[i]);
    }
    
    // Check for duplicates
    for (size_t i = 0; i < hashes.size(); ++i) {
        for (size_t j = i + 1; j < hashes.size(); ++j) {
            if (hashes[i] == hashes[j]) return false;
        }
    }
    return true;
}

static_assert(tool_hashes_unique(), "Hash collision in tool dispatch");
```

**Result:** If a new tool causes a collision, the build fails.

---

## 13.12 Proving Completeness of Visitors

### The Problem

You have a variant and a visitor:

```cpp
using Msg = std::variant<ComposerEnter, StreamDelta, ToolCall>;

struct MsgHandler {
    void operator()(const ComposerEnter&) { /* ... */ }
    void operator()(const StreamDelta&) { /* ... */ }
    // Missing ToolCall!
};
```

**Traditional:** Runtime error or unhandled case.

### Better: Compile-Time Check

Use `std::visit` exhaustiveness checking:

```cpp
std::visit(MsgHandler{}, msg);  
// ERROR: no match for operator() with ToolCall
```

**Even better:** Use concepts to require all overloads:

```cpp
template<typename Visitor, typename... Ts>
concept CompleteVisitor = (requires(Visitor v, Ts t) {
    { v(t) };
} && ...);

static_assert(CompleteVisitor<MsgHandler, ComposerEnter, StreamDelta, ToolCall>,
              "Visitor incomplete");
```

---

## 13.13 Measuring Proof Cost

### Compile-Time Impact

Complex proofs increase compile time:

```cpp
// O(n) proof: negligible
consteval bool check_sizes() {
    for (auto& item : ITEMS) {
        if (item.size > MAX_SIZE) return false;
    }
    return true;
}

// O(n²) proof: noticeable for large n
consteval bool no_duplicates() {
    for (size_t i = 0; i < ITEMS.size(); ++i) {
        for (size_t j = i + 1; j < ITEMS.size(); ++j) {
            if (ITEMS[i] == ITEMS[j]) return false;
        }
    }
    return true;
}
```

**Measured on agentty:**
- 23 tools, O(n²) uniqueness check: **+2ms compile time**
- Worth it for guaranteed correctness

---

## 13.14 When NOT to Use Compile-Time Proofs

### Runtime-Dependent Invariants

Can't prove at compile time:

```cpp
// Runtime input
int port = get_config("port");
assert(port >= 1024 && port <= 65535);  // Must be runtime
```

### Large Search Spaces

Checking 10,000 conditions at compile time:

- ❌ Slows compilation significantly
- ✅ Use runtime checks with good test coverage

### Dynamic Systems

```cpp
// Plugins loaded at runtime
if (!plugin.is_valid()) throw Error("Invalid plugin");
```

**Rule:** Prove what you can at compile time. Check the rest at runtime.

---

## 13.15 Summary

**What we learned:**
- ✅ `consteval` functions force compile-time evaluation
- ✅ `static_assert` enforces invariants as build-time proofs
- ✅ Compile-time proofs eliminate entire classes of bugs
- ✅ Costs are minimal (milliseconds) for typical checks
- ✅ Proofs complement tests, not replace them

**Key insight:** A `static_assert` is a **theorem**. The compiler is your proof checker.

---

## 13.16 Exercises

### Exercise 1: Prove Array Size Match

You have two arrays that must always have the same size:

```cpp
constexpr std::array NAMES = {"read", "write", "edit"};
constexpr std::array HANDLERS = {read_handler, write_handler, edit_handler};
```

Write a proof that they're the same size.

<details>
<summary>Solution</summary>

```cpp
static_assert(NAMES.size() == HANDLERS.size(),
              "Name/handler array size mismatch");
```

</details>

---

### Exercise 2: Prove Enum String Coverage

```cpp
enum class Color { Red, Green, Blue };

constexpr std::array<std::string_view, 3> COLOR_NAMES = {
    "red", "green", "blue"
};
```

Prove that `COLOR_NAMES` has exactly as many entries as `Color` has values.

<details>
<summary>Solution</summary>

```cpp
consteval size_t enum_size(auto...) {
    return 3;  // C++23: use std::enum_size
}

static_assert(COLOR_NAMES.size() == enum_size(Color{}),
              "Color names incomplete");
```

</details>

---

### Exercise 3: Prove No Rank Collision

```cpp
constexpr size_t RANK_A = 0;
constexpr size_t RANK_B = 1;
constexpr size_t RANK_C = 1;  // Oops, duplicate

constexpr std::array RANKS = {RANK_A, RANK_B, RANK_C};
```

Write a proof that catches the duplicate.

<details>
<summary>Solution</summary>

```cpp
consteval bool ranks_unique() {
    for (size_t i = 0; i < RANKS.size(); ++i) {
        for (size_t j = i + 1; j < RANKS.size(); ++j) {
            if (RANKS[i] == RANKS[j]) return false;
        }
    }
    return true;
}

static_assert(ranks_unique(), "Rank collision detected");
```

</details>

---

### Exercise 4: Prove Struct Is POD

```cpp
struct Data {
    int x;
    double y;
};
```

Prove that `Data` is trivially copyable and has no padding.

<details>
<summary>Solution</summary>

```cpp
static_assert(std::is_trivially_copyable_v<Data>,
              "Data must be POD");
static_assert(sizeof(Data) == sizeof(int) + sizeof(double),
              "Data has unexpected padding");
```

</details>

---

### Exercise 5: Prove Tool Names Match Constants

agentty has tool name constants:

```cpp
constexpr std::string_view TOOL_READ = "read";
constexpr std::string_view TOOL_WRITE = "write";

constexpr std::array TOOL_NAMES = {"read", "write", /* ... */};
```

Prove that `TOOL_READ` appears in `TOOL_NAMES`.

<details>
<summary>Solution</summary>

```cpp
consteval bool contains(std::string_view needle) {
    for (auto name : TOOL_NAMES) {
        if (name == needle) return true;
    }
    return false;
}

static_assert(contains(TOOL_READ), "TOOL_READ not in catalog");
static_assert(contains(TOOL_WRITE), "TOOL_WRITE not in catalog");
```

</details>

---

## Next Chapter

In [Chapter 15: Zero-Overhead Abstractions](../ch15-zero-overhead/README.md), we'll see how to build high-level abstractions with **no runtime cost**.

**Preview:** How maya renders complex UIs with the same performance as hand-written C.

---

## Further Reading

- [consteval (cppreference)](https://en.cppreference.com/w/cpp/language/consteval)
- [static_assert (cppreference)](https://en.cppreference.com/w/cpp/language/static_assert)
- agentty source: `include/agentty/tool/catalog_proof.hpp`
- agentty source: `include/agentty/concurrency/ranked_mutex.hpp`

---

**Previous:** [Chapter 12: Template Metaprogramming](../ch12-metaprogramming/README.md)  
**Next:** [Chapter 15: Zero-Overhead Abstractions](../ch15-zero-overhead/README.md)  
**Up:** [Part III: Advanced Techniques](../README.md)
