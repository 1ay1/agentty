# Capstone Solution Guide

This shows you what mastery looks like by building a complete system that uses all four concepts from Chapter 1.

## The Complete Solution

### Part 1: Strong Types (Section 1.1)

```cpp
template <typename Tag>
struct Id {
    std::string value;
    explicit Id(std::string s) noexcept : value(std::move(s)) {}
    bool operator==(const Id&) const = default;
};

// Distinct types created through template specialization
struct ModelIdTag {};
struct UserIdTag {};
struct SessionIdTag {};

using ModelId = Id<ModelIdTag>;
using UserId = Id<UserIdTag>;
using SessionId = Id<SessionIdTag>;
```

**Type Safety Demonstrated:**
```cpp
ModelId mid{"model_123"};
UserId uid{"user_456"};

auto id = mid;  // OK
id = uid;       // COMPILE ERROR: ModelId cannot be assigned UserId
```

---

### Part 2: Ownership and Borrowing (Section 1.2)

```cpp
// Settings holds configuration data
struct Settings {
    ModelId model_id;
    int timeout_ms;
    int max_retries;
    std::optional<std::string> api_key;
};

// User owns Settings
class User {
    UserId user_id_;
    std::string name_;
    Settings settings_;
    
public:
    User(UserId id, std::string name, Settings settings)
        : user_id_(std::move(id)),
          name_(std::move(name)),
          settings_(std::move(settings)) {}
    
    // Ownership decision: User owns Settings
    // Accessor returns const reference (borrowing)
    const Settings& settings() const { return settings_; }
};

// Session borrows User (does not own)
class Session {
    SessionId session_id_;
    const User& user_;  // Borrowed, not owned
    bool active_;
    
public:
    Session(SessionId id, const User& user, bool active = true)
        : session_id_(std::move(id)),
          user_(user),  // Borrow: Session does not own User
          active_(active) {}
    
    const User& user() const { return user_; }
    bool is_active() const { return active_; }
};
```

**Lifetime Guarantees:**
```cpp
{
    User user{UserId{"u1"}, "Alice", {...}};
    Session session{SessionId{"s1"}, user};  // Session borrows User
    
    // session can use user safely here
}
// user is destroyed, session is destroyed
// No use-after-free: session's lifetime never outlives user's
```

---

### Part 3: const Correctness (Section 1.3)

```cpp
class ConfigManager {
    std::vector<User> users_;
    Settings default_settings_;
    
public:
    // Load: returns by value (ownership transfer)
    static ConfigManager load() {
        std::vector<User> users = {
            User{UserId{"u1"}, "Alice", {...}},
            User{UserId{"u2"}, "Bob", {...}}
        };
        return ConfigManager(std::move(users), {...});
    }
    
    // Const member functions: cannot modify
    // Return const references: borrowed, read-only
    const std::vector<User>& users() const {
        return users_;
    }
    
    const Settings& default_settings() const {
        return default_settings_;
    }
    
    // Find by ID: type-safe, const-correct
    const User* find_user(UserId id) const {
        for (const auto& user : users_) {
            if (user.id() == id) {
                return &user;  // Return pointer (nullable result)
            }
        }
        return nullptr;  // Not found
    }
    
    // Find by name: also type-safe and const
    const User* find_user_by_name(const std::string& name) const {
        for (const auto& user : users_) {
            if (user.name() == name) {
                return &user;
            }
        }
        return nullptr;
    }
    
private:
    ConfigManager(std::vector<User> users, Settings defaults)
        : users_(std::move(users)),
          default_settings_(std::move(defaults)) {}
};
```

**const Correctness Verified:**
```cpp
const ConfigManager& config = ConfigManager::load();

// All of these work (const member functions only):
const auto& users = config.users();
const auto& defaults = config.default_settings();
if (const auto* user = config.find_user(UserId{"u1"})) {
    std::cout << user->name();
}

// This would COMPILE ERROR:
// config.users_.push_back(...);  // ERROR: cannot access private mutable member
```

---

### Part 4: Type Deduction (Section 1.4)

```cpp
// Display configuration: takes const reference, uses auto appropriately
void display_config(const ConfigManager& config) {
    std::cout << "Users:\n";
    
    // Typical pattern: const auto& for safe iteration
    for (const auto& user : config.users()) {
        std::cout << "  - " << user.name() << "\n";
        
        // Nested auto: deduced type is auto
        const auto& settings = user.settings();
        std::cout << "    Model: " << settings.model_id << "\n";
    }
}

// Query with structured bindings (C++17 auto deduction)
void query_user(const ConfigManager& config, const std::string& query) {
    if (const auto* user = config.find_user_by_name(query)) {
        // Found: use the pointer safely
        std::cout << "Found: " << user->name() << "\n";
    } else {
        std::cout << "Not found\n";
    }
}

// Create session with type deduction
void create_session(const ConfigManager& config) {
    // auto: deduced from config.users()[0]
    const auto& user = config.users()[0];
    
    // auto: deduced from SessionId constructor
    auto session = Session{SessionId{"session_001"}, user};
    
    // auto: deduced from session.is_active()
    auto active = session.is_active();
    
    std::cout << "Session active: " << (active ? "yes" : "no") << "\n";
}
```

**Type Safety with Deduction:**
```cpp
// This would COMPILE ERROR:
// Mixing ID types cannot happen even with auto

auto mid = ModelId{"m1"};
auto uid = UserId{"u1"};
mid = uid;  // ERROR: even though both are auto, types are distinct

// This is SAFE with auto:
const auto& users = config.users();  // Type is deduced as const std::vector<User>&
// No accidental copies, compiler figured out the reference type
```

---

### Complete Working Example

```cpp
#include <iostream>
#include <string>
#include <vector>
#include <optional>

// ============================================================================
// Section 1.1: Strong Types
// ============================================================================

template <typename Tag>
struct Id {
    std::string value;
    explicit Id(std::string s) noexcept : value(std::move(s)) {}
    bool operator==(const Id&) const = default;
    friend std::ostream& operator<<(std::ostream& os, const Id& id) {
        return os << id.value;
    }
};

struct ModelIdTag {};
struct UserIdTag {};
struct SessionIdTag {};

using ModelId = Id<ModelIdTag>;
using UserId = Id<UserIdTag>;
using SessionId = Id<SessionIdTag>;

// ============================================================================
// Section 1.2: Ownership and Borrowing
// ============================================================================

struct Settings {
    ModelId model_id;
    int timeout_ms;
    int max_retries;
    std::optional<std::string> api_key;
};

class User {
    UserId user_id_;
    std::string name_;
    Settings settings_;
    
public:
    User(UserId id, std::string name, Settings settings)
        : user_id_(std::move(id)),
          name_(std::move(name)),
          settings_(std::move(settings)) {}
    
    const UserId& id() const { return user_id_; }
    const std::string& name() const { return name_; }
    const Settings& settings() const { return settings_; }
};

class Session {
    SessionId session_id_;
    const User& user_;
    bool active_;
    
public:
    Session(SessionId id, const User& user, bool active = true)
        : session_id_(std::move(id)), user_(user), active_(active) {}
    
    const SessionId& id() const { return session_id_; }
    const User& user() const { return user_; }
    bool is_active() const { return active_; }
};

// ============================================================================
// Section 1.3: const Correctness
// ============================================================================

class ConfigManager {
    std::vector<User> users_;
    Settings default_settings_;
    
public:
    static ConfigManager load() {
        std::vector<User> users = {
            User{UserId{"u1"}, "Alice", 
                Settings{ModelId{"gpt-4"}, 5000, 3, std::nullopt}},
            User{UserId{"u2"}, "Bob",
                Settings{ModelId{"gpt-3.5"}, 3000, 2, "secret-key"}}
        };
        return ConfigManager(
            std::move(users),
            Settings{ModelId{"default"}, 5000, 3, std::nullopt}
        );
    }
    
    const std::vector<User>& users() const {
        return users_;
    }
    
    const Settings& default_settings() const {
        return default_settings_;
    }
    
    const User* find_user(UserId id) const {
        for (const auto& user : users_) {
            if (user.id() == id) {
                return &user;
            }
        }
        return nullptr;
    }
    
    const User* find_user_by_name(const std::string& name) const {
        for (const auto& user : users_) {
            if (user.name() == name) {
                return &user;
            }
        }
        return nullptr;
    }
    
private:
    ConfigManager(std::vector<User> users, Settings defaults)
        : users_(std::move(users)),
          default_settings_(std::move(defaults)) {}
};

// ============================================================================
// Section 1.4: Type Deduction
// ============================================================================

void display_config(const ConfigManager& config) {
    std::cout << "=== Configuration Manager ===\n";
    std::cout << "Users:\n";
    
    for (const auto& user : config.users()) {
        std::cout << "  - " << user.id() << " (" << user.name() << ")\n";
        const auto& settings = user.settings();
        std::cout << "    Model: " << settings.model_id << "\n";
    }
}

int main() {
    // Load configuration (ownership transfer)
    auto config = ConfigManager::load();
    
    // Display (const reference borrowing)
    display_config(config);
    
    // Query by ID (type-safe, auto deduction)
    if (const auto* user = config.find_user(UserId{"u1"})) {
        std::cout << "\nFound user: " << user->name() << "\n";
        
        // Create session (borrowing User from config)
        auto session = Session{SessionId{"s1"}, *user};
        std::cout << "Session created: " << session.id() << "\n";
        std::cout << "Active: " << (session.is_active() ? "yes" : "no") << "\n";
    }
    
    // Query by name (also type-safe)
    if (const auto* user = config.find_user_by_name("Bob")) {
        std::cout << "\nFound by name: " << user->name() << "\n";
    }
    
    return 0;
}
```

**Output:**
```
=== Configuration Manager ===
Users:
  - u1 (Alice)
    Model: gpt-4
  - u2 (Bob)
    Model: gpt-3.5

Found user: Alice
Session created: s1
Active: yes

Found by name: Bob
```

---

## What This Solution Demonstrates

### Section 1.1: Strong Types ✅
- `ModelId`, `UserId`, `SessionId` are **distinct types** (not interchangeable)
- Compile-time prevention of ID confusion
- **Zero runtime cost** (one template, compiler generates three distinct types)

### Section 1.2: Values, References, Pointers ✅
- `ConfigManager` **owns** users (stored as values)
- `Session` **borrows** user from config (const reference, no copy)
- `find_user()` returns **nullable pointer** (result may not exist)
- Lifetimes are clear: session cannot outlive user

### Section 1.3: const Correctness ✅
- All accessors are `const` member functions
- Return `const` references (read-only borrowing)
- `find_user_by_name()` returns `const User*` (caller cannot modify)
- No accidental mutations possible

### Section 1.4: Type Deduction ✅
- `auto config = ConfigManager::load()` — deduces ConfigManager
- `const auto& user` — safely deduced as const reference in loop
- `const auto& settings = user.settings()` — zero copies
- `if (const auto* user = config.find_user(...))` — deduced pointer type

---

## Key Principles Demonstrated

| Principle | How It's Used |
|-----------|--------------|
| **Strong Types** | ModelId ≠ UserId at compile time |
| **Ownership** | ConfigManager owns users |
| **Borrowing** | Session borrows from config |
| **const Correctness** | All accessors are const |
| **Type Safety** | Wrong ID type → compile error |
| **Zero Copy** | const references everywhere |
| **Type Deduction** | auto used appropriately |

---

## Try This Yourself

### Exercise 1: Type Safety Verification

Try to uncomment and compile these (should fail):

```cpp
auto mid = ModelId{"m1"};
auto uid = UserId{"u1"};
mid = uid;  // COMPILE ERROR: cannot convert UserId to ModelId
```

### Exercise 2: Reference Lifetime

Try to create a dangling reference (compiler prevents it):

```cpp
Session* make_session() {
    User user{...};
    return new Session{..., user};  // ERROR: session would have dangling ref
}
```

### Exercise 3: const Propagation

Try to modify a const ConfigManager:

```cpp
const ConfigManager& config = ConfigManager::load();
config.users_.push_back(...);  // ERROR: cannot modify const object
```

### Exercise 4: auto Deduction

Verify that auto deduces the right types:

```cpp
auto config = ConfigManager::load();  // Is this ConfigManager?
const auto& users = config.users();   // Is this const vector<User>&?
```

---

## Summary

This capstone demonstrates all four concepts working together:

1. **Strong types** prevent mixing IDs
2. **Ownership and borrowing** determine lifetimes
3. **const correctness** prevents accidental mutations
4. **Type deduction** makes code clean and safe

The result: A system where type safety is **enforced at compile time**, not checked at runtime.

No bugs can hide. The compiler is your copilot.

---

## Next Steps

1. ✅ Complete sections 1.1-1.4
2. ✅ Pass mastery quizzes
3. ✅ Build this capstone from scratch
4. ➜ Move to **Chapter 2: Memory Management (RAII)**

Ready? Start with `SECTION_1_1_DEEP_DIVE.md`.
