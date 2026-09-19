# Chapter 3: Templates — Compile-Time Polymorphism

**Goal:** Master templates for generic programming and zero-runtime-cost abstractions.

**Time:** 5-6 hours  
**Prerequisites:** Chapters 1-2

---

## 3.1 Function Templates: Generic Functions

### The Problem: Code Duplication

Without templates, you'd write the same logic multiple times:

```cpp
int max_int(int a, int b) {
    return (a > b) ? a : b;
}

double max_double(double a, double b) {
    return (a > b) ? a : b;
}

std::string max_string(const std::string& a, const std::string& b) {
    return (a > b) ? a : b;
}
```

**Problems:**
- Duplicate logic (3× the code)
- Harder to maintain (fix bug in 3 places)
- Can't work with new types

### The Solution: Function Templates

```cpp
template <typename T>
T max(T a, T b) {
    return (a > b) ? a : b;
}

// Usage:
int i = max(10, 20);           // T = int
double d = max(3.14, 2.71);    // T = double
std::string s = max("hello", "world");  // T = const char* (careful!)
```

**How it works:**

1. **Template declaration** — `template <typename T>` says "T is a type parameter"
2. **Template instantiation** — Compiler generates code for each type used
3. **Zero runtime cost** — Same as writing three separate functions

### Template Instantiation Under the Hood

When you write:
```cpp
int x = max(10, 20);
double y = max(3.14, 2.71);
```

The compiler generates:
```cpp
// Instantiation for int
int max<int>(int a, int b) {
    return (a > b) ? a : b;
}

// Instantiation for double  
double max<double>(double a, double b) {
    return (a > b) ? a : b;
}
```

**Key insight:** Templates are **compile-time code generation**. Each type gets its own specialized function.

### Template Argument Deduction

The compiler can usually infer template arguments:

```cpp
template <typename T>
T add(T a, T b) {
    return a + b;
}

// Implicit: compiler deduces T = int
int x = add(10, 20);

// Explicit: you specify T
int y = add<int>(10, 20);

// Mixed types don't work without explicit conversion
int z = add(10, 3.14);  // ERROR: T can't be both int and double
int w = add<int>(10, 3.14);  // OK: converts 3.14 to int (3)
```

### Real Example from agentty: Generic ID Generation

```cpp
// include/agentty/domain/id.hpp

template <typename Tag>
struct Id {
    std::string value;
    
    explicit Id(std::string s) noexcept : value(std::move(s)) {}
    
    bool operator==(const Id&) const = default;
    auto operator<=>(const Id&) const = default;
};

// Generate random ID (works for any Tag)
template <typename Tag>
Id<Tag> generate_id() {
    static std::mt19937 rng{std::random_device{}()};
    static std::uniform_int_distribution<uint64_t> dist;
    
    uint64_t id = dist(rng);
    std::ostringstream oss;
    oss << std::hex << std::setw(16) << std::setfill('0') << id;
    return Id<Tag>{oss.str()};
}

// Usage:
ThreadId tid = generate_id<ThreadIdTag>();
MessageId mid = generate_id<MessageIdTag>();
// Same code, different types, compile-time safe
```

---

## 3.2 Class Templates: Generic Data Structures

### Basic Class Template

```cpp
template <typename T>
class Stack {
    std::vector<T> data_;
    
public:
    void push(T value) {
        data_.push_back(std::move(value));
    }
    
    T pop() {
        T value = std::move(data_.back());
        data_.pop_back();
        return value;
    }
    
    bool empty() const {
        return data_.empty();
    }
    
    std::size_t size() const {
        return data_.size();
    }
};

// Usage:
Stack<int> int_stack;
int_stack.push(42);
int_stack.push(100);
int x = int_stack.pop();  // 100

Stack<std::string> string_stack;
string_stack.push("hello");
string_stack.push("world");
std::string s = string_stack.pop();  // "world"
```

### Multiple Template Parameters

```cpp
template <typename Key, typename Value>
class HashMap {
    std::unordered_map<Key, Value> data_;
    
public:
    void insert(Key k, Value v) {
        data_[std::move(k)] = std::move(v);
    }
    
    Value* find(const Key& k) {
        auto it = data_.find(k);
        if (it != data_.end()) {
            return &it->second;
        }
        return nullptr;
    }
};

// Usage:
HashMap<std::string, int> ages;
ages.insert("Alice", 30);
ages.insert("Bob", 25);

if (int* age = ages.find("Alice")) {
    std::cout << *age;  // 30
}
```

### Template Default Arguments

```cpp
template <typename T, typename Container = std::vector<T>>
class Stack {
    Container data_;
    
public:
    void push(T value) {
        data_.push_back(std::move(value));
    }
    
    T pop() {
        T value = std::move(data_.back());
        data_.pop_back();
        return value;
    }
};

// Usage:
Stack<int> s1;  // Uses std::vector<int>
Stack<int, std::deque<int>> s2;  // Uses std::deque<int>
```

### Real Example from maya: Generic Element Wrapper

```cpp
// maya/include/maya/element/box.hpp

template <typename... Children>
struct Box {
    std::vector<Element> children;
    Layout layout;
    Style style;
    
    Box(Children&&... children) 
        : children{std::forward<Children>(children)...} {}
};

// Variadic template allows any number of children
auto my_box = Box{
    text("Hello"),
    text("World"),
    text("From"),
    text("Maya")
};
```

---

## 3.3 Template Specialization: Customizing Behavior

### Full Specialization

**Generic template:**
```cpp
template <typename T>
struct Serializer {
    static std::string serialize(const T& value) {
        // Default: use std::to_string
        return std::to_string(value);
    }
};

// Usage:
int x = 42;
std::string s = Serializer<int>::serialize(x);  // "42"
```

**Specialization for std::string:**
```cpp
template <>
struct Serializer<std::string> {
    static std::string serialize(const std::string& value) {
        // Wrap in quotes
        return "\"" + value + "\"";
    }
};

// Usage:
std::string str = "hello";
std::string s = Serializer<std::string>::serialize(str);  // "\"hello\""
```

**Specialization for bool:**
```cpp
template <>
struct Serializer<bool> {
    static std::string serialize(bool value) {
        return value ? "true" : "false";
    }
};

// Usage:
bool b = true;
std::string s = Serializer<bool>::serialize(b);  // "true"
```

### Partial Specialization (Class Templates Only)

**Generic container serializer:**
```cpp
template <typename T>
struct Serializer<std::vector<T>> {
    static std::string serialize(const std::vector<T>& vec) {
        std::string result = "[";
        for (std::size_t i = 0; i < vec.size(); ++i) {
            if (i > 0) result += ", ";
            result += Serializer<T>::serialize(vec[i]);
        }
        result += "]";
        return result;
    }
};

// Usage:
std::vector<int> nums = {1, 2, 3};
std::string s = Serializer<std::vector<int>>::serialize(nums);
// s == "[1, 2, 3]"

std::vector<std::string> strs = {"a", "b"};
std::string s2 = Serializer<std::vector<std::string>>::serialize(strs);
// s2 == "[\"a\", \"b\"]"
```

### Real Example from agentty: Hash Specialization for Id<Tag>

```cpp
// include/agentty/domain/id.hpp

template <typename Tag>
struct Id {
    std::string value;
    // ...
};

// Specialize std::hash for any Id<Tag>
namespace std {
    template <typename Tag>
    struct hash<agentty::Id<Tag>> {
        std::size_t operator()(const agentty::Id<Tag>& id) const noexcept {
            return std::hash<std::string>{}(id.value);
        }
    };
}

// Now Id<Tag> can be used in unordered_map
std::unordered_map<ThreadId, Thread> thread_cache;
thread_cache[tid] = thread;  // Hash works!
```

---

## 3.4 Variadic Templates: Arbitrary Number of Arguments

### Basic Variadic Template

```cpp
// Base case: no arguments
void print() {
    std::cout << '\n';
}

// Recursive case: one argument + rest
template <typename T, typename... Args>
void print(T first, Args... rest) {
    std::cout << first << ' ';
    print(rest...);  // Recursive call with remaining args
}

// Usage:
print(1, 2, 3, 4, 5);
// Prints: 1 2 3 4 5

print("hello", 42, 3.14, true);
// Prints: hello 42 3.14 1
```

**How it works:**

```
print(1, 2, 3) expands to:
  std::cout << 1 << ' ';
  print(2, 3);
  
print(2, 3) expands to:
  std::cout << 2 << ' ';
  print(3);
  
print(3) expands to:
  std::cout << 3 << ' ';
  print();
  
print() prints newline (base case)
```

### Fold Expressions (C++17)

Modern alternative to recursion:

```cpp
template <typename... Args>
auto sum(Args... args) {
    return (args + ...);  // Fold with +
}

int x = sum(1, 2, 3, 4, 5);  // 15
```

**Fold expression types:**

```cpp
// Unary right fold: (E op ...)
(args + ...)        // a + (b + (c + d))

// Unary left fold: (... op E)
(... + args)        // ((a + b) + c) + d

// Binary right fold: (E op ... op init)
(args + ... + 0)    // a + (b + (c + (d + 0)))

// Binary left fold: (init op ... op E)
(0 + ... + args)    // (((0 + a) + b) + c) + d
```

**Real examples:**

```cpp
// Logical AND
template <typename... Args>
bool all(Args... args) {
    return (args && ...);
}
bool x = all(true, true, false);  // false

// Print all with spaces
template <typename... Args>
void print(Args... args) {
    ((std::cout << args << ' '), ...);
    std::cout << '\n';
}
print(1, 2, 3);  // 1 2 3

// Count arguments
template <typename... Args>
constexpr std::size_t count(Args...) {
    return sizeof...(Args);
}
constexpr std::size_t n = count(1, 2, 3, 4);  // 4
```

### Real Example from agentty: Variant Construction

```cpp
// Nested variant from Chapter 6
namespace msg {
    using ComposerMsg = std::variant<
        ComposerEnter,
        ComposerBackspace,
        ComposerSubmit
    >;
    
    using StreamMsg = std::variant<
        StreamTextDelta,
        StreamFinished
    >;
}

using Msg = std::variant<
    msg::ComposerMsg,
    msg::StreamMsg
>;

// Can construct Msg from any leaf type
Msg m1 = ComposerEnter{};      // Wraps in ComposerMsg, then Msg
Msg m2 = StreamTextDelta{};    // Wraps in StreamMsg, then Msg
```

**How does this work?** std::variant's variadic template constructor:

```cpp
template <typename... Types>
class variant {
public:
    // Variadic constructor: accepts any type in Types...
    template <typename T>
    constexpr variant(T&& value);
    
    // ...
};
```

---

## 3.5 Real Example: agentty's Id<Tag> System Deep Dive

### The Complete Implementation

```cpp
// include/agentty/domain/id.hpp

// Tag-based newtype pattern
template <typename Tag>
struct Id {
    std::string value;
    
    // Explicit constructor prevents accidental string conversions
    explicit constexpr Id(std::string s) noexcept 
        : value(std::move(s)) {}
    
    // Default comparison operators (C++20)
    constexpr bool operator==(const Id&) const = default;
    constexpr auto operator<=>(const Id&) const = default;
    
    // Accessor
    constexpr const std::string& str() const noexcept {
        return value;
    }
};

// Tag types (empty structs, only used for compile-time distinction)
struct ThreadIdTag {};
struct MessageIdTag {};
struct ToolCallIdTag {};
struct ModelIdTag {};

// Type aliases
using ThreadId   = Id<ThreadIdTag>;
using MessageId  = Id<MessageIdTag>;
using ToolCallId = Id<ToolCallIdTag>;
using ModelId    = Id<ModelIdTag>;
```

### Why This Design?

**1. Type safety at zero cost:**
```cpp
// sizeof(ThreadId) == sizeof(std::string)
static_assert(sizeof(ThreadId) == sizeof(std::string));

// No runtime overhead
void process_thread(ThreadId id) {
    // id.value is a direct member access (inline)
    std::string& s = id.value;
}
```

**2. Compile-time error detection:**
```cpp
ThreadId tid{"abc"};
MessageId mid{"xyz"};

void save_thread(ThreadId id);

save_thread(tid);  // OK
save_thread(mid);  // ERROR: cannot convert MessageId to ThreadId
save_thread("abc");  // ERROR: explicit constructor
```

**3. Tag types are zero-size:**
```cpp
static_assert(sizeof(ThreadIdTag) == 1);  // Empty class
static_assert(std::is_empty_v<ThreadIdTag>);

// Tag is only used at compile time, never instantiated
Id<ThreadIdTag> id{"abc"};
// ThreadIdTag doesn't exist in compiled code
```

**4. Works with standard containers:**
```cpp
// Hash specialization (see §3.3)
std::unordered_map<ThreadId, Thread> cache;

// Ordering works via operator<=>
std::set<MessageId> seen_messages;
std::map<ModelId, ModelInfo> models;
```

### Template Magic: How One Template Generates Many Types

```cpp
// This ONE template definition:
template <typename Tag>
struct Id { std::string value; };

// Generates FOUR distinct types:
Id<ThreadIdTag>   // ThreadId
Id<MessageIdTag>  // MessageId  
Id<ToolCallIdTag> // ToolCallId
Id<ModelIdTag>    // ModelId

// Each is a completely separate type
static_assert(!std::is_same_v<ThreadId, MessageId>);

// But all share the same implementation
static_assert(sizeof(ThreadId) == sizeof(MessageId));
```

**Compiler output (simplified):**

```cpp
// The compiler generates four instantiations:

struct Id_ThreadIdTag {
    std::string value;
    explicit constexpr Id_ThreadIdTag(std::string s) noexcept;
    // ...
};

struct Id_MessageIdTag {
    std::string value;
    explicit constexpr Id_MessageIdTag(std::string s) noexcept;
    // ...
};

// ... and so on
```

### Advanced: Constrained Id Template

```cpp
// Require Tag to be empty (optimization check)
template <typename Tag>
    requires std::is_empty_v<Tag>
struct Id {
    std::string value;
    // ...
};

// This compiles:
using ThreadId = Id<ThreadIdTag>;  // ThreadIdTag is empty

// This wouldn't:
// using BadId = Id<int>;  // ERROR: int is not empty
```

---

## 3.6 Template Compilation Model: Why Templates Are in Headers

### The Problem

**Normal function (can separate declaration and definition):**

```cpp
// math.hpp
int add(int a, int b);

// math.cpp
int add(int a, int b) {
    return a + b;
}

// main.cpp
#include "math.hpp"
int x = add(1, 2);  // Linker finds add() in math.cpp
```

**Template function (CANNOT separate):**

```cpp
// math.hpp
template <typename T>
T add(T a, T b);  // Declaration only

// math.cpp
template <typename T>
T add(T a, T b) {  // Definition
    return a + b;
}

// main.cpp
#include "math.hpp"
int x = add(1, 2);  // ERROR: Linker can't find add<int>
```

**Why?** The compiler needs to see the template **definition** to instantiate it for `int`.

### The Solution: Definitions in Headers

```cpp
// math.hpp
template <typename T>
T add(T a, T b) {  // Definition in header
    return a + b;
}

// main.cpp
#include "math.hpp"
int x = add(1, 2);  // OK: compiler generates add<int>
```

### Real Example from agentty

```cpp
// include/agentty/domain/id.hpp
// ENTIRE Id<Tag> implementation is in the header

template <typename Tag>
struct Id {
    std::string value;
    
    explicit constexpr Id(std::string s) noexcept 
        : value(std::move(s)) {}
    
    // All members defined inline
    constexpr bool operator==(const Id&) const = default;
    constexpr auto operator<=>(const Id&) const = default;
};

// Used in many .cpp files:
// src/runtime/app/update.cpp
// src/io/persistence.cpp
// src/tool/tools.cpp
// Each sees the full definition and instantiates as needed
```

### Compilation Cost

**Templates can increase compile times:**

```cpp
// big_template.hpp
template <typename T>
struct BigClass {
    // 1000 lines of code
};

// file1.cpp
#include "big_template.hpp"
BigClass<int> x;  // Instantiates 1000 lines

// file2.cpp
#include "big_template.hpp"
BigClass<int> y;  // Instantiates 1000 lines AGAIN

// file3.cpp
#include "big_template.hpp"
BigClass<int> z;  // Instantiates 1000 lines AGAIN
```

**Solution: Extern templates (C++11):**

```cpp
// big_template.hpp
template <typename T>
struct BigClass { /* ... */ };

extern template struct BigClass<int>;  // Don't instantiate here

// big_template.cpp
template struct BigClass<int>;  // Instantiate once

// file1.cpp, file2.cpp, file3.cpp
#include "big_template.hpp"
BigClass<int> x;  // Uses pre-instantiated version
```

**agentty doesn't use extern templates** because:
1. Most templates are small (Id<Tag>, overload)
2. LTO eliminates duplicate instantiations
3. Simplicity > compile-time optimization

---

## 3.7 Exercises

### Exercise 3.1: Generic Swap

Implement a generic swap function:

```cpp
template <typename T>
void swap(T& a, T& b) {
    // TODO: Implement
}

// Test:
int x = 10, y = 20;
swap(x, y);
assert(x == 20 && y == 10);

std::string s1 = "hello", s2 = "world";
swap(s1, s2);
assert(s1 == "world" && s2 == "hello");
```

**Starter code:** `exercises/ch03/ex1-swap.cpp`  
**Solution:** `solutions/ch03/ex1-swap.cpp`

### Exercise 3.2: Generic Container Find

Implement a find function that works with any container:

```cpp
template <typename Container, typename Value>
auto find(const Container& c, const Value& v) {
    // TODO: Return iterator to first occurrence, or end()
}

// Test:
std::vector<int> vec = {1, 2, 3, 4, 5};
auto it = find(vec, 3);
assert(*it == 3);

std::list<std::string> lst = {"a", "b", "c"};
auto it2 = find(lst, "b");
assert(*it2 == "b");
```

**Starter code:** `exercises/ch03/ex2-find.cpp`  
**Solution:** `solutions/ch03/ex2-find.cpp`

### Exercise 3.3: Implement Id<Tag> from Scratch

Implement your own version of Id<Tag>:

```cpp
template <typename Tag>
struct MyId {
    // TODO: Implement
    // - std::string value member
    // - Explicit constructor
    // - Equality operators
    // - Hash specialization
};

// Test:
using UserId = MyId<struct UserIdTag>;
using SessionId = MyId<struct SessionIdTag>;

UserId uid{"user123"};
SessionId sid{"sess456"};

// This should NOT compile:
// void process(UserId id);
// process(sid);  // ERROR
```

**Starter code:** `exercises/ch03/ex3-my-id.cpp`  
**Solution:** `solutions/ch03/ex3-my-id.cpp`

### Exercise 3.4: Variadic Min

Implement a variadic min function that returns the smallest of any number of arguments:

```cpp
template <typename... Args>
auto min(Args... args) {
    // TODO: Use fold expression
}

// Test:
assert(min(5, 2, 8, 1, 9) == 1);
assert(min(3.14, 2.71, 1.41) == 1.41);
```

**Hint:** You can't fold with `<`, but you can with `?:` or by unpacking into an array.

**Starter code:** `exercises/ch03/ex4-variadic-min.cpp`  
**Solution:** `solutions/ch03/ex4-variadic-min.cpp`

### Exercise 3.5: Generic Pair

Implement a generic Pair class that holds two values of potentially different types:

```cpp
template <typename T1, typename T2>
struct Pair {
    // TODO: Implement
    // - first, second members
    // - Constructor
    // - Equality operator
};

// Test:
Pair<int, std::string> p{42, "hello"};
assert(p.first == 42);
assert(p.second == "hello");

Pair<int, int> p2{1, 2};
Pair<int, int> p3{1, 2};
assert(p2 == p3);
```

**Starter code:** `exercises/ch03/ex5-pair.cpp`  
**Solution:** `solutions/ch03/ex5-pair.cpp`

### Exercise 3.6: Build a Simple Registry

Implement a registry that stores objects by ID:

```cpp
template <typename IdType, typename ValueType>
class Registry {
    // TODO: Use std::unordered_map<IdType, ValueType>
    // Implement:
    // - insert(IdType id, ValueType value)
    // - find(IdType id) -> ValueType*
    // - remove(IdType id) -> bool
    // - size() -> std::size_t
};

// Test with strong IDs:
using UserId = Id<struct UserIdTag>;
using User = std::string;

Registry<UserId, User> users;
users.insert(UserId{"u1"}, "Alice");
users.insert(UserId{"u2"}, "Bob");

assert(*users.find(UserId{"u1"}) == "Alice");
assert(users.size() == 2);
```

**Starter code:** `exercises/ch03/ex6-registry.cpp`  
**Solution:** `solutions/ch03/ex6-registry.cpp`

---

## Key Takeaways

1. **Templates are compile-time code generation**
   - One template → many type-specific functions/classes
   - Zero runtime overhead

2. **Function templates enable generic algorithms**
   - Write once, use with any type
   - Compiler deduces types automatically

3. **Class templates enable generic data structures**
   - Stack<T>, HashMap<K, V>, Id<Tag>
   - Type safety with zero cost

4. **Specialization customizes behavior**
   - Full specialization: one specific type
   - Partial specialization: category of types

5. **Variadic templates handle arbitrary arguments**
   - Pack expansion, fold expressions
   - Enables print(), sum(), etc.

6. **Tag-based newtypes are a powerful pattern**
   - Id<Tag> prevents ID type confusion
   - Compile-time safety, zero runtime cost
   - Used extensively in agentty

7. **Template definitions go in headers**
   - Compiler needs full definition to instantiate
   - Can increase compile times
   - Extern templates for frequently-used types

---

## Next Chapter

[Chapter 4: Move Semantics and Perfect Forwarding →](../ch04-move/README.md)

In the next chapter, you'll learn:
- Lvalues and rvalues
- Move constructors and move assignment
- std::move and std::forward
- Return Value Optimization (RVO)
- How agentty's update() function transfers ownership efficiently
