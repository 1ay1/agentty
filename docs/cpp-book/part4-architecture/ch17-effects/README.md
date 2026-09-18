# Chapter 17: Effect Systems and Pure Functions

**Learning Objectives:**
- Understand side effects and why they complicate testing
- Model effects as **data**, not actions
- Implement `Cmd<Msg>` for describing I/O
- Build interpreters that execute effects
- See how maya's effect system works
- Write testable, pure update functions

---

## 17.1 The Problem with Side Effects

### What Is a Side Effect?

Any operation that interacts with the outside world:

- File I/O (read, write)
- Network requests (HTTP, WebSocket)
- Console output (print, log)
- Time (current timestamp, sleep)
- Randomness (random numbers)
- Global state (modify globals)

### Why Side Effects Are Hard

**Example:**

```cpp
void handle_message(const std::string& msg) {
    auto response = http_post("https://api.com/chat", msg);  // Network I/O
    save_to_file("history.txt", response);                   // File I/O
    std::cout << "Sent: " << msg << '\n';                    // Console I/O
}
```

**Problems:**
1. **Untestable** — can't test without real network/files
2. **Non-deterministic** — network might fail, file might not exist
3. **Hard to reason about** — what if HTTP fails? What order do effects happen?
4. **Impossible to replay** — can't rerun without side effects

---

## 17.2 Pure Functions: The Solution

### Definition

A **pure function**:
1. Same input → same output (deterministic)
2. No side effects (doesn't modify external state)
3. No hidden dependencies (doesn't read global state)

**Example:**

```cpp
// Pure
int add(int a, int b) { return a + b; }

// Impure (side effect)
void print_sum(int a, int b) {
    std::cout << (a + b) << '\n';  // Console I/O
}

// Impure (hidden dependency)
int get_random() {
    return rand();  // Reads global RNG state
}
```

### Benefits of Pure Functions

1. **Easy to test** — no mocks needed
2. **Easy to understand** — just look at inputs/outputs
3. **Composable** — can combine freely
4. **Parallelizable** — no shared state
5. **Cacheable** — same inputs → same outputs

---

## 17.3 Side Effects as Data

### The Key Idea

Don't **perform** side effects. **Describe** them as data:

```cpp
// Bad: performs effect
void send_message(const std::string& msg) {
    http_post("https://api.com/chat", msg);  // Effect happens here
}

// Good: describes effect
struct HttpPost {
    std::string url;
    std::string body;
};

HttpPost send_message(const std::string& msg) {
    return {"https://api.com/chat", msg};  // Just data
}
```

**Now:**
- `send_message()` is **pure** (no I/O)
- Returns a **description** of what to do
- Something else interprets the description

---

## 17.4 The Cmd<Msg> Pattern

### From The Elm Architecture

Elm introduced `Cmd msg` — a type representing **commands** (effects) that produce messages:

```elm
-- Elm pseudocode
update : Msg -> Model -> (Model, Cmd Msg)
update msg model =
    case msg of
        SendRequest text ->
            ( { model | loading = True }
            , http_post "api/chat" text  -- Returns Cmd Msg
            )
```

**Key insight:** `update()` is **pure**. It returns:
1. New model (data)
2. Command to execute (data)

The **runtime** interprets commands and feeds results back as messages.

---

## 17.5 Cmd<Msg> in C++

### The Type

`maya/include/maya/core/cmd.hpp`:

```cpp
template<typename Msg>
class Cmd {
public:
    // Factory: HTTP request
    static Cmd http_get(std::string url) {
        return Cmd{HttpGet{std::move(url)}};
    }
    
    // Factory: delay
    static Cmd delay(std::chrono::milliseconds ms, Msg msg) {
        return Cmd{Delay{ms, msg}};
    }
    
    // Factory: task (background work)
    template<typename F>
    static Cmd task(F&& work) {
        return Cmd{Task{std::forward<F>(work)}};
    }
    
    // Factory: none (no effect)
    static Cmd none() {
        return Cmd{None{}};
    }
    
private:
    using Effect = std::variant<None, HttpGet, Delay, Task, /* ... */>;
    Effect effect_;
    
    explicit Cmd(Effect e) : effect_(std::move(e)) {}
};
```

---

## 17.6 Pure Update Function

### agentty's Core Loop

`agentty/src/runtime/app/update.cpp`:

```cpp
UpdateResult update(Model model, const Msg& msg) {
    return std::visit(overload{
        [&](const ComposerEnter& m) -> UpdateResult {
            // Pure state update
            model.composer.active = true;
            model.composer.cursor = 0;
            
            // No side effects here!
            return {std::move(model), Cmd<Msg>::none()};
        },
        
        [&](const SubmitPrompt& m) -> UpdateResult {
            // Update state
            model.is_streaming = true;
            
            // Describe effect (don't perform it)
            auto cmd = Cmd<Msg>::task([prompt = m.text]() {
                return StreamStart{make_request(prompt)};
            });
            
            return {std::move(model), std::move(cmd)};
        },
        
        [&](const StreamTextDelta& m) -> UpdateResult {
            // Accumulate streaming text
            model.current_response += m.delta;
            return {std::move(model), Cmd<Msg>::none()};
        }
        
    }, msg);
}
```

**Key properties:**
- ✅ Pure function (no I/O, no globals)
- ✅ Testable (just call with model + message)
- ✅ Deterministic (same inputs → same outputs)
- ✅ Effects as data (`Cmd<Msg>`)

---

## 17.7 Effect Interpretation

### The Runtime

`maya/src/runtime/background_queue.cpp`:

```cpp
void BackgroundQueue::interpret(Cmd<Msg> cmd) {
    std::visit(overload{
        [](Cmd<Msg>::None) {
            // No-op
        },
        
        [this](Cmd<Msg>::Task& t) {
            // Run task on thread pool
            pool_.enqueue([work = std::move(t.work), this]() {
                auto msg = work();  // Execute
                dispatch(msg);      // Send result back
            });
        },
        
        [this](Cmd<Msg>::HttpGet& h) {
            // HTTP request
            http_client_.get(h.url, [this](Result<Response> r) {
                auto msg = r.has_value() 
                    ? HttpSuccess{r.value()}
                    : HttpFailed{r.error()};
                dispatch(msg);
            });
        },
        
        [this](Cmd<Msg>::Delay& d) {
            // Schedule delayed message
            timer_.schedule(d.duration, [msg = d.msg, this]() {
                dispatch(msg);
            });
        }
        
    }, cmd.effect());
}
```

**Separation of concerns:**
- `update()`: business logic (pure)
- `interpret()`: effect execution (impure)

---

## 17.8 Batching Commands

### Multiple Effects

Sometimes you need multiple effects:

```cpp
UpdateResult update(Model model, const Msg& msg) {
    return std::visit(overload{
        [&](const SaveAndQuit& m) -> UpdateResult {
            auto save_cmd = Cmd<Msg>::task([]() {
                save_state();
                return SaveComplete{};
            });
            
            auto quit_cmd = Cmd<Msg>::delay(100ms, QuitApp{});
            
            // Batch: both effects run
            auto batch = Cmd<Msg>::batch({
                std::move(save_cmd),
                std::move(quit_cmd)
            });
            
            return {std::move(model), std::move(batch)};
        }
    }, msg);
}
```

**Implementation:**

```cpp
template<typename Msg>
class Cmd {
    struct Batch {
        std::vector<Cmd<Msg>> commands;
    };
    
    using Effect = std::variant<None, Task, Batch, /* ... */>;
    
public:
    static Cmd batch(std::vector<Cmd> cmds) {
        return Cmd{Batch{std::move(cmds)}};
    }
};
```

---

## 17.9 Real Example: Tool Execution

### The Problem

agentty executes tools like `read`, `write`, `shell`. Each is an effect:

```cpp
// Bad: impure
std::string execute_tool(const ToolCall& call) {
    if (call.name == "read") {
        return read_file(call.args["path"]);  // File I/O
    }
    if (call.name == "shell") {
        return run_command(call.args["command"]);  // Process execution
    }
    // ...
}
```

**Problems:**
- Can't test without real filesystem/shell
- Can't mock for unit tests
- Can't replay execution

### The Solution: Cmd-Based Tool Execution

```cpp
UpdateResult update(Model model, const Msg& msg) {
    return std::visit(overload{
        [&](const ExecuteTool& m) -> UpdateResult {
            // Pure: describe the effect
            auto cmd = Cmd<Msg>::task([call = m.call]() {
                auto result = tool::execute(call);
                return ToolComplete{call.id, result};
            });
            
            model.active_tools.insert(m.call.id);
            return {std::move(model), std::move(cmd)};
        }
    }, msg);
}
```

**Now:**
- `update()` is pure (no tool execution)
- Effect happens in background (via `interpret()`)
- Result comes back as a message (`ToolComplete`)
- Easy to test: call `update()` with `ExecuteTool`, check returned `Cmd`

---

## 17.10 Testing Pure Update Functions

### Example Test

```cpp
TEST(Update, SubmitPromptStartsStreaming) {
    // Arrange
    Model model;
    model.is_streaming = false;
    
    SubmitPrompt msg{"Hello, AI"};
    
    // Act
    auto [new_model, cmd] = update(std::move(model), msg);
    
    // Assert
    EXPECT_TRUE(new_model.is_streaming);
    EXPECT_TRUE(std::holds_alternative<Cmd<Msg>::Task>(cmd.effect()));
    
    // No actual HTTP request happened!
}
```

**Benefits:**
- Fast (no I/O)
- Deterministic (no flakiness)
- Focused (tests business logic, not I/O)

---

## 17.11 Effect Composition

### Sequential Effects

```cpp
// Run effect A, then effect B with A's result
auto cmd = Cmd<Msg>::task([]() { return fetch_data(); })
    .and_then([](Data d) { return Cmd<Msg>::task([d]() { return process(d); }); });
```

**Implementation:**

```cpp
template<typename Msg>
class Cmd {
public:
    template<typename F>
    Cmd and_then(F&& f) {
        return Cmd{AndThen{
            std::move(*this),
            std::forward<F>(f)
        }};
    }
};
```

---

## 17.12 When NOT to Use Effect Systems

### Overhead

Effect systems add complexity:

```cpp
// Simple (but impure)
void handle_click() {
    std::cout << "Clicked\n";
}

// Effect system (pure, but verbose)
UpdateResult handle_click(Model model) {
    auto cmd = Cmd<Msg>::task([]() {
        std::cout << "Clicked\n";
        return ClickLogged{};
    });
    return {model, cmd};
}
```

**When to skip:**
- Tiny programs (< 1000 lines)
- Scripts (run once, not tested)
- Performance-critical inner loops

**When to use:**
- Large applications (agentty: 40K lines)
- Need testability
- Complex state management

---

## 17.13 Summary

**What we learned:**
- ✅ Side effects make code hard to test and reason about
- ✅ Pure functions are deterministic and easy to test
- ✅ Model effects as **data** with `Cmd<Msg>`
- ✅ Separate logic (`update`) from execution (`interpret`)
- ✅ agentty's update function is 100% pure
- ✅ Effects can be batched, composed, tested

**Key insight:** Make effects **explicit** in the type system. Your business logic should be pure.

---

## 17.14 Exercises

### Exercise 1: Pure vs Impure

Identify which functions are pure:

```cpp
int square(int x) { return x * x; }
void log(std::string msg) { std::cout << msg; }
int get_time() { return time(nullptr); }
std::string uppercase(std::string s) { /* converts to upper */ }
```

---

### Exercise 2: Describe Effects as Data

Rewrite this impure function to return a command:

```cpp
void send_email(std::string to, std::string body) {
    smtp_client.send(to, body);  // Network I/O
}
```

---

### Exercise 3: Implement Cmd::batch

```cpp
template<typename Msg>
class Cmd {
    // YOUR CODE
public:
    static Cmd batch(std::vector<Cmd> cmds);
};
```

---

### Exercise 4: Test Pure Update

Write a test for this update function:

```cpp
UpdateResult update(Model model, const IncrementCounter& msg) {
    model.counter += msg.amount;
    return {model, Cmd<Msg>::none()};
}
```

---

## Next Chapter

In [Chapter 18: Algebraic Data Types](../ch18-adt/README.md), we'll formalize sum types, product types, and build recursive data structures.

---

**Previous:** [Chapter 15: Zero-Overhead Abstractions](../../part3-advanced/ch15-zero-overhead/README.md)  
**Next:** [Chapter 18: Algebraic Data Types](../ch18-adt/README.md)  
**Up:** [Part IV: Architecture](../README.md)
