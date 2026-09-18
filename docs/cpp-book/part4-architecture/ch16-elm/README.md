# Chapter 16: The Elm Architecture in C++

**Goal:** Master functional architecture for interactive applications.

**Time:** 5-6 hours  
**Prerequisites:** Chapters 1-15

---

## 16.1 What Is The Elm Architecture?

**The Elm Architecture (TEA)** is a pattern for building interactive programs as pure functions.

### The Three Core Principles

1. **Model** — All application state in one immutable value
2. **Update** — Pure function: `(Model, Msg) → (Model, Cmd)`
3. **View** — Pure function: `Model → UI`

**Key insight:** Your program is a function from events to UI updates, with explicit side effects.

### The Runtime Loop

```
┌─────────────────────────────────────┐
│          Runtime Loop               │
│                                     │
│  Model m = init();                  │
│  while (running) {                  │
│      Event e = wait_event();        │
│      Msg msg = translate(e);        │
│      (m, cmd) = update(m, msg);     │
│      execute(cmd);                  │
│      render(view(m));               │
│  }                                  │
└─────────────────────────────────────┘
```

**Benefits:**
- **Testable** — update() is pure, needs no mocks
- **Debuggable** — Every state transition is a value
- **Time-travel** — Store (Model, Msg) pairs, replay them
- **Crash-safe** — No half-updated state

---

## 16.2 Model, Msg, Update, View

### The Model: All Application State

```cpp
// agentty's Model (simplified)
struct Model {
    // Domain state
    Thread              thread;
    std::vector<Thread> history;
    provider::Selection active_provider;
    
    // UI state
    std::string         composer_text;
    int                 composer_cursor;
    panel::Stack        panel_stack;
    
    // Ephemeral state
    struct Stream {
        std::string partial_text;
        std::chrono::time_point started_at;
    } stream;
};
```

**Rules:**
1. **One Model** — No scattered state
2. **Value semantics** — Copy to snapshot, no shared_ptr
3. **No I/O** — Model is pure data

### The Msg: Every Event

```cpp
// agentty's Msg (simplified)
namespace msg {
    // Composer messages
    struct ComposerEnter {};
    struct ComposerBackspace {};
    struct ComposerSubmit {};
    
    // Stream messages
    struct StreamTextDelta { std::string text; };
    struct StreamToolUse { ToolCallId id; std::string name; json args; };
    struct StreamFinished { StreamEnd end; };
    
    // ... 200+ more message types
}

using Msg = std::variant<
    msg::ComposerMsg,
    msg::StreamMsg,
    msg::ThreadListMsg,
    // ... 10 domain variants
>;
```

**Rules:**
1. **Closed sum type** — All events are Msg variants
2. **Immutable** — Messages are values, never modified
3. **Descriptive** — Name tells you what happened

### The Update Function: State Transitions

```cpp
// Pure function: same inputs → same outputs
std::pair<Model, Cmd<Msg>> update(Model m, Msg msg) {
    return std::visit(overload{
        [&](msg::ComposerEnter) -> std::pair<Model, Cmd<Msg>> {
            if (m.composer_text.empty()) {
                return {std::move(m), Cmd<Msg>::none()};
            }
            
            // Transition to streaming state
            m.stream.started_at = std::chrono::steady_clock::now();
            
            // Side effect: launch HTTP stream
            Cmd<Msg> cmd = Cmd<Msg>::task([text = std::move(m.composer_text)] {
                return stream_request(text);
            });
            
            // Clear composer
            m.composer_text.clear();
            m.composer_cursor = 0;
            
            return {std::move(m), std::move(cmd)};
        },
        
        [&](msg::StreamTextDelta delta) -> std::pair<Model, Cmd<Msg>> {
            m.stream.partial_text += delta.text;
            return {std::move(m), Cmd<Msg>::none()};
        },
        
        [&](msg::StreamFinished finish) -> std::pair<Model, Cmd<Msg>> {
            // Finalize message
            Message msg;
            msg.id = generate_id();
            msg.role = Role::Assistant;
            msg.text = std::move(m.stream.partial_text);
            m.thread.messages.push_back(std::move(msg));
            
            // Reset stream state
            m.stream = {};
            
            // Side effect: persist thread
            Cmd<Msg> cmd = Cmd<Msg>::task([t = m.thread] {
                save_thread(t);
                return std::nullopt;  // No resulting message
            });
            
            return {std::move(m), std::move(cmd)};
        },
        
        // ... 197 more handlers
        
    }, msg);
}
```

**Rules:**
1. **Pure** — No I/O, no global state
2. **Moves Model** — Takes by value, returns new value
3. **Returns Cmd** — Describes side effects to perform

### The View Function: Render UI

```cpp
// Pure function: Model → UI
maya::Element view(const Model& m) {
    using namespace maya;
    
    return v(  // vertical stack
        render_thread(m.thread),
        render_stream(m.stream),
        render_composer(m.composer_text, m.composer_cursor),
        render_statusbar(m)
    );
}

Element render_composer(const std::string& text, int cursor) {
    return Composer::Config{
        .text = text,
        .cursor = cursor,
        .placeholder = "Message...",
        .state = Composer::State::Idle
    };
}
```

**Rules:**
1. **No mutation** — Model is const reference
2. **Deterministic** — Same Model → same UI
3. **No I/O** — Just build data structure

---

## 16.3 Pure Functions for State Transitions

### Why Purity Matters

**Impure (OOP style):**
```cpp
class Application {
    Model model_;
    
    void on_enter() {
        if (model_.composer_text.empty()) return;
        
        // Mutate shared state
        model_.stream.started_at = now();
        
        // Side effect (hard to test)
        http_client_.stream(model_.composer_text, [this](std::string delta) {
            model_.stream.partial_text += delta;
            this->render();
        });
        
        model_.composer_text.clear();
    }
};
```

**Problems:**
1. **Hard to test** — Needs mock HTTP client
2. **Hidden state changes** — model_ mutated in callback
3. **Race conditions** — Callback fires while model_ being read
4. **No undo** — Can't restore previous state

**Pure (TEA style):**
```cpp
std::pair<Model, Cmd<Msg>> on_enter(Model m) {
    if (m.composer_text.empty()) {
        return {std::move(m), Cmd<Msg>::none()};
    }
    
    m.stream.started_at = now();
    
    Cmd<Msg> cmd = Cmd<Msg>::task([text = std::move(m.composer_text)] {
        auto result = http_stream(text);
        return StreamTextDelta{result};
    });
    
    m.composer_text.clear();
    
    return {std::move(m), std::move(cmd)};
}
```

**Benefits:**
1. **Easy to test** — `assert(on_enter(m).first.composer_text.empty())`
2. **Explicit state** — New model returned
3. **No races** — Cmd executed after update returns
4. **Undo for free** — Keep old Model

### Testing Pure Functions

```cpp
// test/update_test.cpp
TEST_CASE("ComposerEnter with empty text does nothing") {
    Model m;
    m.composer_text = "";
    
    auto [m2, cmd] = update(std::move(m), ComposerEnter{});
    
    CHECK(m2.stream.started_at.time_since_epoch().count() == 0);
    CHECK(cmd.is_none());
}

TEST_CASE("ComposerEnter with text starts stream") {
    Model m;
    m.composer_text = "hello";
    
    auto [m2, cmd] = update(std::move(m), ComposerEnter{});
    
    CHECK(m2.composer_text.empty());
    CHECK(m2.stream.started_at.time_since_epoch().count() > 0);
    CHECK(!cmd.is_none());
}
```

**No mocks, no setup, no teardown. Just values in, values out.**

---

## 16.4 Real Example: agentty's Core Loop

### The Full Architecture

```cpp
// include/agentty/runtime/app/program.hpp

struct AgenttyApp {
    using Model = agentty::Model;
    using Msg = agentty::Msg;
    
    // 1. Initialize
    static std::pair<Model, maya::Cmd<Msg>> init() {
        Model m;
        m.thread = Thread{};
        m.composer_text = "";
        
        maya::Cmd<Msg> cmd = maya::Cmd<Msg>::task([] {
            auto threads = load_recent_threads();
            return ThreadsLoaded{threads};
        });
        
        return {std::move(m), std::move(cmd)};
    }
    
    // 2. Update
    static std::pair<Model, maya::Cmd<Msg>> update(Model m, Msg msg) {
        return agentty::app::update(std::move(m), std::move(msg));
    }
    
    // 3. View
    static maya::Element view(const Model& m) {
        return agentty::app::view(m);
    }
    
    // 4. Subscribe (timers, signals)
    static std::vector<maya::Sub<Msg>> subscriptions(const Model& m) {
        std::vector<maya::Sub<Msg>> subs;
        
        if (m.stream.started_at.time_since_epoch().count() > 0) {
            // Poll stream every 100ms
            subs.push_back(maya::Sub<Msg>::interval(
                std::chrono::milliseconds{100},
                [] { return StreamPoll{}; }
            ));
        }
        
        return subs;
    }
};
```

### The Runtime

```cpp
// maya/src/runtime/runtime.cpp (simplified)

template <typename App>
int run() {
    using Model = typename App::Model;
    using Msg = typename App::Msg;
    
    auto [model, init_cmd] = App::init();
    execute(init_cmd);
    
    while (running) {
        // Wait for terminal input or timer
        std::optional<Event> event = wait_for_event();
        
        if (!event) continue;
        
        // Translate event to message
        Msg msg = translate(*event);
        
        // Update model
        auto [next_model, cmd] = App::update(std::move(model), std::move(msg));
        execute(cmd);
        
        // Render
        Element ui = App::view(next_model);
        render_to_terminal(ui);
        
        model = std::move(next_model);
    }
    
    return 0;
}
```

**Key properties:**
1. **Single-threaded** — Update runs on UI thread
2. **Synchronous** — One message at a time
3. **Ordered** — Messages processed in sequence

### How Side Effects Work

```cpp
// Cmd<Msg> is a description of side effects
template <typename Msg>
struct Cmd {
    enum class Kind { None, Task, Batch };
    
    struct TaskCmd {
        std::function<std::optional<Msg>()> fn;
    };
    
    struct BatchCmd {
        std::vector<Cmd> cmds;
    };
    
    using Payload = std::variant<std::monostate, TaskCmd, BatchCmd>;
    
    Payload payload;
    
    // Smart constructors
    static Cmd none() { return Cmd{}; }
    
    static Cmd task(std::function<std::optional<Msg>()> fn) {
        return Cmd{TaskCmd{std::move(fn)}};
    }
    
    static Cmd batch(std::vector<Cmd> cmds) {
        return Cmd{BatchCmd{std::move(cmds)}};
    }
};
```

**Executing Cmd:**
```cpp
template <typename Msg>
void execute(Cmd<Msg> cmd) {
    std::visit(overload{
        [](std::monostate) {
            // No-op
        },
        [](TaskCmd task) {
            // Run on worker thread
            std::thread([fn = std::move(task.fn)] {
                if (auto msg = fn()) {
                    dispatch(*msg);  // Send result back to UI thread
                }
            }).detach();
        },
        [](BatchCmd batch) {
            for (auto& c : batch.cmds) {
                execute(std::move(c));
            }
        },
    }, cmd.payload);
}
```

---

## 16.5 Exercises

### Exercise 16.1: Counter App

Implement a simple counter in TEA style:

```cpp
struct Model {
    int count = 0;
};

struct Increment {};
struct Decrement {};
struct Reset {};

using Msg = std::variant<Increment, Decrement, Reset>;

std::pair<Model, Cmd<Msg>> update(Model m, Msg msg);
std::string view(const Model& m);
```

**Starter code:** `exercises/ch16/ex1-counter.cpp`  
**Solution:** `solutions/ch16/ex1-counter.cpp`

### Exercise 16.2: Todo List

Implement a todo list:

```cpp
struct Todo {
    int id;
    std::string text;
    bool done;
};

struct Model {
    std::vector<Todo> todos;
    std::string input;
};

struct AddTodo {};
struct ToggleTodo { int id; };
struct DeleteTodo { int id; };
struct UpdateInput { std::string text; };

using Msg = std::variant<AddTodo, ToggleTodo, DeleteTodo, UpdateInput>;

std::pair<Model, Cmd<Msg>> update(Model m, Msg msg);
```

**Starter code:** `exercises/ch16/ex2-todo.cpp`  
**Solution:** `solutions/ch16/ex2-todo.cpp`

### Exercise 16.3: HTTP Fetch

Add async HTTP fetching:

```cpp
struct Model {
    std::optional<std::string> data;
    bool loading = false;
};

struct FetchData {};
struct DataReceived { std::string data; };
struct FetchFailed { std::string error; };

using Msg = std::variant<FetchData, DataReceived, FetchFailed>;

std::pair<Model, Cmd<Msg>> update(Model m, Msg msg);
```

**Starter code:** `exercises/ch16/ex3-fetch.cpp`  
**Solution:** `solutions/ch16/ex3-fetch.cpp`

### Exercise 16.4: Mini Text Editor

Implement a line-based text editor with undo:

```cpp
struct Model {
    std::vector<std::string> lines;
    int cursor_line;
    int cursor_col;
    std::vector<Model> history;  // For undo
};

struct InsertChar { char ch; };
struct DeleteChar {};
struct Newline {};
struct Undo {};

using Msg = std::variant<InsertChar, DeleteChar, Newline, Undo>;

std::pair<Model, Cmd<Msg>> update(Model m, Msg msg);
```

**Starter code:** `exercises/ch16/ex4-editor.cpp`  
**Solution:** `solutions/ch16/ex4-editor.cpp`

---

## Key Takeaways

1. **TEA = Pure Functions + Explicit Effects**
   - Model: all state
   - Msg: all events
   - Update: (Model, Msg) → (Model, Cmd)
   - View: Model → UI

2. **Purity enables testing**
   - No mocks
   - No setup/teardown
   - Just values in, values out

3. **Cmd describes side effects**
   - Runtime interprets them
   - Update function stays pure

4. **Move semantics for efficiency**
   - Pass Model by value
   - Move into/out of functions
   - RVO eliminates copies

5. **agentty is 100% TEA**
   - 423K LOC
   - Zero global mutable state
   - All state transitions are pure functions

6. **Benefits at scale**
   - Time-travel debugging
   - Hot code reload
   - Easy refactoring
   - Fearless concurrency

---

## Next Chapter

[Chapter 17: Effect Systems and Pure Functions →](../ch17-effects/README.md)

In the next chapter, you'll learn:
- Cmd<Msg> implementation details
- Task scheduling
- Batching effects
- How maya's runtime works
