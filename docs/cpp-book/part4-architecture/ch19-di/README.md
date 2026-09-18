# Chapter 19: Dependency Injection via Type Erasure

**Learning Objectives:**
- Understand why hard dependencies make testing difficult
- Implement dependency injection in C++
- Use type erasure to avoid templates
- See agentty's `Deps` seam for testing
- Write testable code with fake implementations
- Apply interface segregation

---

## 19.1 The Problem: Hard Dependencies

### Untestable Code

```cpp
class ChatApp {
public:
    void send_message(std::string text) {
        // Hard-coded dependency on HTTP client
        http::Client client;
        auto response = client.post("https://api.openai.com/chat", text);
        save_to_database(response);
    }
};
```

**Problems:**
- Can't test without real network
- Can't test without real database
- Can't mock responses
- Slow tests (network latency)

---

## 19.2 Solution: Dependency Injection

### Inject Dependencies

```cpp
class ChatApp {
    http::Client& client_;
    Database& db_;
    
public:
    ChatApp(http::Client& client, Database& db)
        : client_(client), db_(db) {}
    
    void send_message(std::string text) {
        auto response = client_.post("https://api.openai.com/chat", text);
        db_.save(response);
    }
};
```

**Now:**
- Dependencies passed in (not hard-coded)
- Can inject fake implementations for testing

---

## 19.3 Testing with Fakes

### Fake HTTP Client

```cpp
class FakeHttpClient : public http::Client {
    std::string fake_response_;
    
public:
    FakeHttpClient(std::string response) : fake_response_(response) {}
    
    std::string post(std::string url, std::string body) override {
        return fake_response_;  // No real network
    }
};

TEST(ChatApp, SendMessageSavesResponse) {
    FakeHttpClient fake_client{"AI response"};
    FakeDatabase fake_db;
    
    ChatApp app{fake_client, fake_db};
    app.send_message("Hello");
    
    EXPECT_EQ(fake_db.last_saved(), "AI response");
}
```

**Benefits:**
- Fast (no network)
- Deterministic (controlled responses)
- Focused (tests business logic)

---

## 19.4 The Problem with Interfaces

### Virtual Dispatch Overhead

```cpp
class HttpClient {
public:
    virtual ~HttpClient() = default;
    virtual std::string post(std::string url, std::string body) = 0;
};
```

**Costs:**
- Vtable indirection (2 memory loads)
- Can't inline
- Forces heap allocation

**In hot paths:** 5-10% overhead.

---

## 19.5 Type Erasure: Best of Both Worlds

### The Pattern

Use `std::function` to erase the type:

```cpp
class ChatApp {
    std::function<std::string(std::string, std::string)> http_post_;
    std::function<void(std::string)> db_save_;
    
public:
    ChatApp(
        std::function<std::string(std::string, std::string)> http_post,
        std::function<void(std::string)> db_save
    ) : http_post_(http_post), db_save_(db_save) {}
    
    void send_message(std::string text) {
        auto response = http_post_("https://api.openai.com/chat", text);
        db_save_(response);
    }
};
```

**Benefits:**
- No inheritance
- Lambdas can be injected
- Easier testing

---

## 19.6 Real Example: agentty's Deps Seam

### The Deps Type

`agentty/include/agentty/runtime/deps.hpp`:

```cpp
struct Deps {
    // HTTP client
    std::function<StreamResult(Request)> stream_request;
    
    // File I/O
    std::function<Result<std::string>(std::string path)> read_file;
    std::function<Result<void>(std::string path, std::string content)> write_file;
    
    // Process execution
    std::function<Result<std::string>(std::string cmd)> run_command;
    
    // Persistence
    std::function<Result<Thread>(ThreadId)> load_thread;
    std::function<Result<void>(Thread)> save_thread;
    
    // Time
    std::function<int64_t()> now_ms;
};
```

### Production Dependencies

`agentty/src/runtime/deps.cpp`:

```cpp
Deps make_production_deps() {
    return Deps{
        .stream_request = [](Request req) {
            return provider::stream(req);  // Real HTTP
        },
        .read_file = [](std::string path) {
            return io::read_file(path);  // Real file I/O
        },
        .write_file = [](std::string path, std::string content) {
            return io::write_file(path, content);
        },
        .run_command = [](std::string cmd) {
            return shell::execute(cmd);  // Real process
        },
        .load_thread = [](ThreadId id) {
            return persistence::load(id);
        },
        .save_thread = [](Thread t) {
            return persistence::save(t);
        },
        .now_ms = []() {
            return std::chrono::system_clock::now().time_since_epoch().count();
        }
    };
}
```

### Test Dependencies

`agentty/tests/fake_deps.hpp`:

```cpp
struct FakeDeps {
    std::unordered_map<std::string, std::string> fake_files;
    std::vector<std::string> executed_commands;
    int64_t fake_time = 0;
    
    Deps make_deps() {
        return Deps{
            .stream_request = [](Request req) {
                return StreamResult::ok();  // Fake success
            },
            .read_file = [this](std::string path) -> Result<std::string> {
                if (auto it = fake_files.find(path); it != fake_files.end()) {
                    return it->second;
                }
                return std::unexpected{Error::FileNotFound};
            },
            .write_file = [this](std::string path, std::string content) -> Result<void> {
                fake_files[path] = content;
                return {};
            },
            .run_command = [this](std::string cmd) -> Result<std::string> {
                executed_commands.push_back(cmd);
                return "command output";
            },
            .load_thread = [](ThreadId id) -> Result<Thread> {
                return Thread{id, {}};
            },
            .save_thread = [](Thread t) -> Result<void> {
                return {};
            },
            .now_ms = [this]() {
                return fake_time;
            }
        };
    }
};
```

### Testing with Fake Deps

```cpp
TEST(ToolExecution, ReadFileReturnsContent) {
    FakeDeps fake;
    fake.fake_files["test.txt"] = "Hello, world!";
    
    auto deps = fake.make_deps();
    
    auto result = deps.read_file("test.txt");
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value(), "Hello, world!");
}

TEST(ToolExecution, WriteFileSavesContent) {
    FakeDeps fake;
    auto deps = fake.make_deps();
    
    deps.write_file("output.txt", "Test content");
    
    EXPECT_EQ(fake.fake_files["output.txt"], "Test content");
}

TEST(ToolExecution, ShellExecutionRecorded) {
    FakeDeps fake;
    auto deps = fake.make_deps();
    
    deps.run_command("ls -la");
    
    ASSERT_EQ(fake.executed_commands.size(), 1);
    EXPECT_EQ(fake.executed_commands[0], "ls -la");
}
```

---

## 19.7 Interface Segregation

### The Problem

One giant interface:

```cpp
struct Deps {
    std::function<StreamResult(Request)> stream_request;
    std::function<Result<std::string>(std::string)> read_file;
    std::function<Result<void>(std::string, std::string)> write_file;
    std::function<Result<std::string>(std::string)> run_command;
    // ... 20 more functions
};
```

**If a function only needs file I/O:**

```cpp
void process(Deps& deps) {
    auto content = deps.read_file("input.txt");
    // Doesn't need stream_request, run_command, etc.
}
```

### Solution: Split Interfaces

```cpp
struct FileDeps {
    std::function<Result<std::string>(std::string)> read_file;
    std::function<Result<void>(std::string, std::string)> write_file;
};

struct NetworkDeps {
    std::function<StreamResult(Request)> stream_request;
};

struct ShellDeps {
    std::function<Result<std::string>(std::string)> run_command;
};

// Full deps compose smaller deps
struct Deps {
    FileDeps file;
    NetworkDeps network;
    ShellDeps shell;
};
```

**Now:**

```cpp
void process(FileDeps& deps) {
    auto content = deps.read_file("input.txt");
    // Only depends on file I/O
}
```

---

## 19.8 Measuring Overhead

### Type Erasure Cost

```cpp
// Direct call
std::string read_file_direct(std::string path) {
    return io::read_file(path);
}

// Through std::function
std::function<std::string(std::string)> read_file_erased = [](std::string path) {
    return io::read_file(path);
};
```

**Benchmark:**
- Direct: 150ns
- Type-erased: 152ns

**Overhead:** ~1-2% (one indirect call).

**Worth it?** Yes, for testability.

---

## 19.9 When NOT to Use Dependency Injection

### Performance-Critical Inner Loops

```cpp
// Bad: DI in hot path
void render_frame(Deps& deps) {
    for (int i = 0; i < 1'000'000; ++i) {
        deps.draw_pixel(i, color);  // Indirect call per pixel
    }
}

// Good: DI at high level
void render_frame(Canvas& canvas) {
    for (int i = 0; i < 1'000'000; ++i) {
        canvas.draw_pixel(i, color);  // Direct call (inlined)
    }
}

// Inject at startup
void main() {
    Canvas canvas = make_canvas(deps);  // DI here
    render_frame(canvas);
}
```

**Rule:** Inject dependencies at **system boundaries**, not in tight loops.

---

## 19.10 Summary

**What we learned:**
- ✅ Hard dependencies make code untestable
- ✅ Dependency injection enables fake implementations
- ✅ Type erasure avoids virtual dispatch
- ✅ `std::function` is perfect for DI
- ✅ agentty's Deps seam isolates I/O
- ✅ Interface segregation keeps deps minimal

**Key insight:** Inject dependencies at **boundaries**. Core logic stays pure and testable.

---

## 19.11 Exercises

### Exercise 1: Inject Logger

```cpp
class Service {
public:
    void process(int x) {
        std::cout << "Processing " << x << '\n';  // Hard-coded
    }
};
```

Refactor to inject a logger.

---

### Exercise 2: Fake HTTP Client

Write a `FakeHttpClient` that returns canned responses for testing.

---

### Exercise 3: Split Deps

```cpp
struct Deps {
    std::function<std::string(std::string)> read_file;
    std::function<void(std::string, std::string)> write_file;
    std::function<StreamResult(Request)> stream_request;
};
```

Split into `FileDeps` and `NetworkDeps`.

---

## Next Chapter

In [Chapter 20: Building a TUI Framework](../ch20-tui/README.md), we'll dive deep into maya's architecture.

---

**Previous:** [Chapter 18: Algebraic Data Types](../ch18-adt/README.md)  
**Next:** [Chapter 20: Building a TUI Framework](../ch20-tui/README.md)  
**Up:** [Part IV: Architecture](../README.md)
