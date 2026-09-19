# Chapter 20: Building a TUI Framework

**Learning Objectives:**
- Understand terminal UI architecture
- Master declarative UI with immutable state
- Implement flex layout (rows, columns, spacing)
- Build reusable components (buttons, inputs, panels)
- See how maya powers agentty's UI
- Achieve 60 FPS rendering

---

## 20.1 Why TUI?

### Terminal User Interfaces

**TUI:** Text-based UI in a terminal (not GUI).

**Examples:**
- vim, emacs
- htop, btop
- tmux, screen
- **agentty**

**Benefits:**
- Fast (no GPU, minimal rendering)
- Remote-friendly (SSH, Mosh)
- Accessible (screen readers)
- Low resource usage

---

## 20.2 The Challenge

### Stateful vs Declarative

**Traditional TUI (stateful):**

```cpp
void render() {
    move_cursor(10, 5);
    print("Hello");
    move_cursor(10, 6);
    print("World");
}
```

**Problems:**
- Hard to reason about (cursor state)
- Manual layout (calculate positions)
- Diff tracking (what changed?)

**Modern TUI (declarative):**

```cpp
auto ui = vstack({
    text("Hello"),
    text("World")
});
```

**Benefits:**
- Describe **what**, not **how**
- Framework handles layout
- Automatic diffing

---

## 20.3 maya's Architecture

### Core Concepts

1. **Element:** Immutable UI description
2. **Layout:** Compute sizes and positions
3. **Render:** Draw to canvas
4. **Diff:** Only redraw changes

**Flow:**

```
App State → Element Tree → Layout → Canvas → Terminal
```

---

## 20.4 Element Tree

### Building UIs

```cpp
#include <maya/maya.hpp>

auto ui = vstack({
    text("Title"),
    hstack({
        button("Save", on_save),
        button("Cancel", on_cancel)
    }),
    input(state.text, on_input)
});
```

**Each element is:**
- **Immutable** (rebuilding is cheap)
- **Composable** (nest arbitrarily)
- **Type-erased** (any element fits)

---

## 20.5 Layout: Flex Model

### Rows and Columns

`maya/include/maya/element/flex.hpp`:

```cpp
// Vertical stack (column)
auto col = vstack({
    elem1,  // Takes natural height
    elem2,
    elem3
});

// Horizontal stack (row)
auto row = hstack({
    elem1,  // Takes natural width
    elem2,
    elem3
});
```

### Flex Properties

```cpp
elem | flex(1)        // Grow to fill space
elem | shrink(0)      // Don't shrink below natural size
elem | basis(20)      // Start at 20 units
elem | width(40)      // Fixed width
elem | height(10)     // Fixed height
elem | padding(2)     // Space inside
elem | gap(1)         // Space between children
```

---

## 20.6 Real Example: agentty's Main Layout

`agentty/src/runtime/view/app_view.cpp`:

```cpp
Element build_ui(const Model& model) {
    return vstack({
        // Header
        header(model),
        
        // Main content (flexible)
        hstack({
            // Thread list (20% width)
            thread_list(model.threads) | width_pct(20),
            
            // Message view (80% width, flexible height)
            message_view(model.current_thread) | flex(1),
        }) | flex(1),  // Grow to fill
        
        // Composer (fixed height)
        composer(model.composer) | height(3),
        
        // Status bar (fixed height)
        status_bar(model) | height(1)
    });
}
```

**Result:**
- Header: top, fixed height
- Thread list: left, 20% width
- Messages: center, fills remaining space
- Composer: bottom, fixed 3 lines
- Status: very bottom, 1 line

---

## 20.7 Component: Button

### The Interface

```cpp
Element button(std::string label, std::function<void()> on_click);
```

### Implementation

`maya/src/element/button.cpp`:

```cpp
Element button(std::string label, std::function<void()> on_click) {
    return text(label)
        | padding(1)
        | border()
        | on_click(std::move(on_click))
        | style(Style::inverse());  // Highlight on hover
}
```

**Modifiers:**
- `padding(1)` — 1 space on each side
- `border()` — box drawing characters
- `on_click()` — register click handler
- `style()` — visual styling

---

## 20.8 Component: Input Field

### The Interface

```cpp
Element input(
    std::string value,
    std::function<void(std::string)> on_change
);
```

### Implementation

`maya/src/element/input.cpp`:

```cpp
struct InputState {
    std::string value;
    size_t cursor;
    bool focused;
};

Element input(std::string value, std::function<void(std::string)> on_change) {
    return custom([value, on_change](Canvas& c, Box region) {
        // Render text
        c.draw_text(region.x, region.y, value);
        
        // Render cursor if focused
        if (focused) {
            c.draw_cell(region.x + cursor, region.y, Cell::cursor());
        }
    })
    | on_key([value, on_change](Key k) mutable {
        if (k.is_char()) {
            value.insert(cursor, 1, k.ch);
            cursor++;
            on_change(value);
        }
        // Handle backspace, arrows, etc.
    })
    | height(1)
    | flex(1);  // Grow horizontally
}
```

---

## 20.9 Rendering Pipeline

### Step 1: Layout

Compute sizes and positions:

```cpp
Box layout(const Element& elem, Size available) {
    return elem.layout(available);
}
```

**Output:** `Box{x, y, width, height}` for each element.

### Step 2: Render

Draw to canvas:

```cpp
void render(const Element& elem, Canvas& canvas, Box region) {
    elem.render(canvas, region);
}
```

**Canvas:** In-memory buffer of cells (codepoint + style).

### Step 3: Diff

Find changes:

```cpp
std::vector<Cell> diff(const Canvas& prev, const Canvas& curr) {
    std::vector<Cell> changes;
    for (size_t i = 0; i < curr.size(); ++i) {
        if (prev[i] != curr[i]) {
            changes.push_back({i, curr[i]});
        }
    }
    return changes;
}
```

### Step 4: Flush

Send changes to terminal:

```cpp
void flush(const std::vector<Cell>& changes) {
    for (auto [pos, cell] : changes) {
        move_cursor(pos.x, pos.y);
        write_cell(cell);
    }
}
```

---

## 20.10 Performance: Incremental Rendering

### The Problem

Rendering 10,000 messages every frame:

```cpp
for (auto& msg : messages) {
    render(msg_view(msg), canvas, region);
}
```

**Cost:** 40ms per frame (25 FPS).

### Solution: Virtualization

Only render **visible** messages:

```cpp
size_t first_visible = scroll_offset / message_height;
size_t last_visible = (scroll_offset + viewport_height) / message_height;

for (size_t i = first_visible; i <= last_visible; ++i) {
    render(msg_view(messages[i]), canvas, region);
}
```

**Cost:** 0.8ms per frame (1250 FPS).

---

## 20.11 Real Example: agentty's Message List

### The Challenge

- 26,000 messages in a thread
- Each message: 10-200 lines
- Can't render all (too slow)

### Solution: Virtual Scrolling

`agentty/src/runtime/view/message_list.cpp`:

```cpp
Element message_list(const Thread& thread, int scroll_offset, int viewport_height) {
    // Compute visible range
    int y = 0;
    size_t first_visible = 0;
    for (size_t i = 0; i < thread.messages.size(); ++i) {
        int msg_height = compute_height(thread.messages[i]);
        if (y + msg_height >= scroll_offset) {
            first_visible = i;
            break;
        }
        y += msg_height;
    }
    
    // Render only visible
    std::vector<Element> visible;
    y = 0;
    for (size_t i = first_visible; i < thread.messages.size(); ++i) {
        auto elem = message_view(thread.messages[i]);
        visible.push_back(elem);
        y += compute_height(thread.messages[i]);
        if (y > viewport_height) break;
    }
    
    return vstack(std::move(visible));
}
```

**Result:**
- 26K messages: 0.9ms per frame
- Smooth 60 FPS scrolling

---

## 20.12 Styling

### The Style System

`maya/include/maya/core/style.hpp`:

```cpp
struct Style {
    Color fg;
    Color bg;
    bool bold;
    bool italic;
    bool underline;
    
    static Style default_style() {
        return {Color::default_fg(), Color::default_bg(), false, false, false};
    }
    
    static Style inverse() {
        return {Color::default_bg(), Color::default_fg(), false, false, false};
    }
};
```

### Applying Styles

```cpp
text("Error") | fg(Color::red()) | bold()
text("Success") | fg(Color::green())
text("Link") | fg(Color::blue()) | underline()
```

---

## 20.13 Event Handling

### The Event Loop

```cpp
while (running) {
    auto event = terminal.read_event();
    
    auto msg = std::visit(overload{
        [](KeyEvent k) { return Msg{KeyPressed{k}}; },
        [](MouseEvent m) { return Msg{MouseClicked{m}}; },
        [](ResizeEvent r) { return Msg{WindowResized{r}}; }
    }, event);
    
    auto [new_model, cmd] = update(model, msg);
    model = std::move(new_model);
    interpret(std::move(cmd));
    
    render(build_ui(model));
}
```

---

## 20.14 Summary

**What we learned:**
- ✅ Declarative UI with immutable elements
- ✅ Flex layout (rows, columns, grow/shrink)
- ✅ Virtual scrolling for large lists
- ✅ Incremental rendering (diff + patch)
- ✅ Event loop (read, update, render)
- ✅ 60 FPS performance

**Key insight:** Rebuild the entire UI every frame. Diffing makes it fast.

---

## 20.15 Exercises

### Exercise 1: Layout

Build a 3-column layout:

```cpp
// Left: 20%, Center: 60%, Right: 20%
```

---

### Exercise 2: Virtual Scroll

Implement virtual scrolling for a 1000-item list.

---

### Exercise 3: Custom Component

Build a `progress_bar(float percent)` component.

---

## Next Chapter

In [Chapter 21: Thread Persistence](../../part5-case-studies/ch21-thread-persist/README.md), we'll see how agentty stores threads on disk.

---

**Previous:** [Chapter 19: Dependency Injection](../ch19-di/README.md)  
**Next:** [Chapter 21: Thread Persistence](../../part5-case-studies/ch21-thread-persist/README.md)  
**Up:** [Part IV: Architecture](../README.md)
