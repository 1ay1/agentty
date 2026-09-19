# Chapter 25: Building Your Own Terminal Agent

**Learning Objectives:**
- Apply everything learned in this book
- Build a minimal AI coding agent
- Implement tool calling (read, write, shell)
- Create a simple TUI
- Use The Elm Architecture
- Deploy your agent

---

## 25.1 Project Overview

### What We'll Build

**mini-agent:** A terminal AI coding assistant with:

1. **Chat interface** (send prompts, get responses)
2. **Tool calling** (read files, write files, run commands)
3. **Streaming responses** (real-time output)
4. **Thread persistence** (save conversations)
5. **Simple TUI** (terminal UI)

**Stack:**
- **Provider:** Anthropic Claude (simplest API)
- **HTTP:** libcurl (or your choice)
- **JSON:** nlohmann/json
- **TUI:** Raw ANSI escape codes (no framework)

---

## 25.2 Project Structure

```
mini-agent/
├── CMakeLists.txt
├── src/
│   ├── main.cpp
│   ├── provider.hpp         # Anthropic API
│   ├── tools.hpp            # read, write, shell
│   ├── update.hpp           # Elm Architecture
│   └── view.hpp             # Terminal rendering
├── tests/
│   └── test_tools.cpp
└── README.md
```

---

## 25.3 Step 1: The Model

### State

`src/model.hpp`:

```cpp
#pragma once
#include <string>
#include <vector>

struct Message {
    enum class Role { User, Assistant, Tool };
    Role role;
    std::string content;
    std::optional<std::string> tool_call_id;
};

struct Model {
    std::vector<Message> messages;
    std::string current_input;
    bool is_streaming = false;
    std::string stream_buffer;
};
```

---

## 25.4 Step 2: Messages

### The Msg Type

`src/msg.hpp`:

```cpp
#pragma once
#include <string>
#include <variant>

struct InputChar { char ch; };
struct Submit {};
struct StreamDelta { std::string text; };
struct StreamEnd {};
struct ToolCall { std::string name; nlohmann::json args; };
struct ToolResult { std::string tool_call_id; std::string result; };
struct Quit {};

using Msg = std::variant<
    InputChar,
    Submit,
    StreamDelta,
    StreamEnd,
    ToolCall,
    ToolResult,
    Quit
>;
```

---

## 25.5 Step 3: Tools

### The Implementation

`src/tools.hpp`:

```cpp
#pragma once
#include <string>
#include <expected>
#include <fstream>

namespace tool {

std::expected<std::string, std::string> read_file(const std::string& path) {
    std::ifstream file(path);
    if (!file) {
        return std::unexpected("File not found: " + path);
    }
    
    std::string content((std::istreambuf_iterator<char>(file)),
                        std::istreambuf_iterator<char>());
    return content;
}

std::expected<std::string, std::string> write_file(
    const std::string& path,
    const std::string& content
) {
    std::ofstream file(path);
    if (!file) {
        return std::unexpected("Cannot write to: " + path);
    }
    
    file << content;
    return "Written " + std::to_string(content.size()) + " bytes";
}

std::expected<std::string, std::string> run_shell(const std::string& command) {
    FILE* pipe = popen(command.c_str(), "r");
    if (!pipe) {
        return std::unexpected("Failed to run: " + command);
    }
    
    std::string result;
    char buffer[128];
    while (fgets(buffer, sizeof(buffer), pipe)) {
        result += buffer;
    }
    
    pclose(pipe);
    return result;
}

// Dispatch by name
std::expected<std::string, std::string> execute(
    const std::string& name,
    const nlohmann::json& args
) {
    if (name == "read_file") {
        return read_file(args["path"]);
    }
    if (name == "write_file") {
        return write_file(args["path"], args["content"]);
    }
    if (name == "shell") {
        return run_shell(args["command"]);
    }
    
    return std::unexpected("Unknown tool: " + name);
}

} // namespace tool
```

---

## 25.6 Step 4: Provider

### Anthropic Streaming

`src/provider.hpp`:

```cpp
#pragma once
#include <string>
#include <functional>
#include <curl/curl.h>
#include <nlohmann/json.hpp>

using EventSink = std::function<void(const std::string&)>;

class Provider {
    std::string api_key_;
    
    static size_t write_callback(char* ptr, size_t size, size_t nmemb, void* userdata) {
        auto* sink = static_cast<EventSink*>(userdata);
        std::string chunk(ptr, size * nmemb);
        (*sink)(chunk);
        return size * nmemb;
    }
    
public:
    explicit Provider(std::string api_key) : api_key_(std::move(api_key)) {}
    
    void stream(const std::vector<Message>& messages, EventSink sink) {
        // Build request body
        nlohmann::json body = {
            {"model", "claude-3-5-sonnet-20241022"},
            {"max_tokens", 4096},
            {"stream", true},
            {"messages", serialize_messages(messages)},
            {"tools", {
                {
                    {"name", "read_file"},
                    {"description", "Read a file"},
                    {"input_schema", {
                        {"type", "object"},
                        {"properties", {{"path", {{"type", "string"}}}}},
                        {"required", {"path"}}
                    }}
                },
                {
                    {"name", "write_file"},
                    {"description", "Write to a file"},
                    {"input_schema", {
                        {"type", "object"},
                        {"properties", {
                            {"path", {{"type", "string"}}},
                            {"content", {{"type", "string"}}}
                        }},
                        {"required", {"path", "content"}}
                    }}
                },
                {
                    {"name", "shell"},
                    {"description", "Run a shell command"},
                    {"input_schema", {
                        {"type", "object"},
                        {"properties", {{"command", {{"type", "string"}}}}},
                        {"required", {"command"}}
                    }}
                }
            }}
        };
        
        // HTTP request
        CURL* curl = curl_easy_init();
        curl_easy_setopt(curl, CURLOPT_URL, "https://api.anthropic.com/v1/messages");
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.dump().c_str());
        
        struct curl_slist* headers = nullptr;
        headers = curl_slist_append(headers, ("x-api-key: " + api_key_).c_str());
        headers = curl_slist_append(headers, "anthropic-version: 2023-06-01");
        headers = curl_slist_append(headers, "content-type: application/json");
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &sink);
        
        curl_easy_perform(curl);
        curl_easy_cleanup(curl);
    }
    
private:
    nlohmann::json serialize_messages(const std::vector<Message>& messages) {
        nlohmann::json result = nlohmann::json::array();
        for (auto& msg : messages) {
            result.push_back({
                {"role", msg.role == Message::Role::User ? "user" : "assistant"},
                {"content", msg.content}
            });
        }
        return result;
    }
};
```

---

## 25.7 Step 5: Update Function

### Pure Logic

`src/update.hpp`:

```cpp
#pragma once
#include "model.hpp"
#include "msg.hpp"
#include <variant>

struct UpdateResult {
    Model model;
    std::optional<std::function<void()>> cmd;
};

UpdateResult update(Model model, const Msg& msg) {
    return std::visit([&](auto&& m) -> UpdateResult {
        using T = std::decay_t<decltype(m)>;
        
        if constexpr (std::is_same_v<T, InputChar>) {
            model.current_input += m.ch;
            return {std::move(model), std::nullopt};
        }
        else if constexpr (std::is_same_v<T, Submit>) {
            // Add user message
            model.messages.push_back({
                Message::Role::User,
                model.current_input,
                std::nullopt
            });
            
            auto input = model.current_input;
            model.current_input.clear();
            model.is_streaming = true;
            
            // Cmd: stream request
            auto cmd = [input, messages = model.messages]() {
                Provider provider(std::getenv("ANTHROPIC_API_KEY"));
                provider.stream(messages, [](const std::string& chunk) {
                    // Parse SSE and dispatch StreamDelta/ToolCall
                    parse_and_dispatch(chunk);
                });
            };
            
            return {std::move(model), cmd};
        }
        else if constexpr (std::is_same_v<T, StreamDelta>) {
            model.stream_buffer += m.text;
            return {std::move(model), std::nullopt};
        }
        else if constexpr (std::is_same_v<T, StreamEnd>) {
            model.messages.push_back({
                Message::Role::Assistant,
                model.stream_buffer,
                std::nullopt
            });
            model.stream_buffer.clear();
            model.is_streaming = false;
            return {std::move(model), std::nullopt};
        }
        else if constexpr (std::is_same_v<T, ToolCall>) {
            // Cmd: execute tool
            auto cmd = [name = m.name, args = m.args]() {
                auto result = tool::execute(name, args);
                if (result.has_value()) {
                    dispatch(ToolResult{"tool-1", *result});
                } else {
                    dispatch(ToolResult{"tool-1", "Error: " + result.error()});
                }
            };
            
            return {std::move(model), cmd};
        }
        else if constexpr (std::is_same_v<T, ToolResult>) {
            model.messages.push_back({
                Message::Role::Tool,
                m.result,
                m.tool_call_id
            });
            return {std::move(model), std::nullopt};
        }
        else if constexpr (std::is_same_v<T, Quit>) {
            std::exit(0);
        }
        
    }, msg);
}
```

---

## 25.8 Step 6: View (TUI)

### Simple Rendering

`src/view.hpp`:

```cpp
#pragma once
#include "model.hpp"
#include <iostream>

void render(const Model& model) {
    // Clear screen
    std::cout << "\033[2J\033[H";
    
    // Render messages
    for (auto& msg : model.messages) {
        if (msg.role == Message::Role::User) {
            std::cout << "\033[1;34mYou:\033[0m " << msg.content << "\n\n";
        } else if (msg.role == Message::Role::Assistant) {
            std::cout << "\033[1;32mAI:\033[0m " << msg.content << "\n\n";
        } else if (msg.role == Message::Role::Tool) {
            std::cout << "\033[1;33m[Tool]:\033[0m " << msg.content << "\n\n";
        }
    }
    
    // Render streaming buffer
    if (model.is_streaming && !model.stream_buffer.empty()) {
        std::cout << "\033[1;32mAI:\033[0m " << model.stream_buffer << "▊\n\n";
    }
    
    // Render input
    std::cout << "\033[1;34m>\033[0m " << model.current_input << "▊";
    std::cout.flush();
}
```

---

## 25.9 Step 7: Main Loop

### The Runtime

`src/main.cpp`:

```cpp
#include "model.hpp"
#include "msg.hpp"
#include "update.hpp"
#include "view.hpp"
#include <termios.h>
#include <unistd.h>

char read_char() {
    struct termios old_tio, new_tio;
    tcgetattr(STDIN_FILENO, &old_tio);
    new_tio = old_tio;
    new_tio.c_lflag &= ~(ICANON | ECHO);
    tcsetattr(STDIN_FILENO, TCSANOW, &new_tio);
    
    char ch = getchar();
    
    tcsetattr(STDIN_FILENO, TCSANOW, &old_tio);
    return ch;
}

int main() {
    Model model;
    
    while (true) {
        render(model);
        
        char ch = read_char();
        
        Msg msg;
        if (ch == '\n') {
            msg = Submit{};
        } else if (ch == 3) {  // Ctrl-C
            msg = Quit{};
        } else {
            msg = InputChar{ch};
        }
        
        auto [new_model, cmd] = update(std::move(model), msg);
        model = std::move(new_model);
        
        if (cmd) {
            // Execute command (in background thread for real impl)
            (*cmd)();
        }
    }
    
    return 0;
}
```

---

## 25.10 Building

### CMakeLists.txt

```cmake
cmake_minimum_required(VERSION 3.20)
project(mini-agent)

set(CMAKE_CXX_STANDARD 23)

find_package(CURL REQUIRED)
find_package(nlohmann_json REQUIRED)

add_executable(mini-agent
    src/main.cpp
)

target_link_libraries(mini-agent
    CURL::libcurl
    nlohmann_json::nlohmann_json
)
```

### Build Commands

```bash
mkdir build
cd build
cmake ..
make
./mini-agent
```

---

## 25.11 Extending the Agent

### Add More Tools

```cpp
// src/tools.hpp
std::expected<std::string, std::string> list_files(const std::string& dir) {
    // Implementation
}

std::expected<std::string, std::string> search_code(const std::string& pattern) {
    // Use ripgrep
}
```

### Add Persistence

```cpp
// src/persistence.hpp
void save_thread(const std::vector<Message>& messages) {
    std::ofstream file("thread.jsonl", std::ios::app);
    for (auto& msg : messages) {
        file << to_json(msg).dump() << '\n';
    }
}

std::vector<Message> load_thread() {
    std::ifstream file("thread.jsonl");
    std::vector<Message> messages;
    std::string line;
    while (std::getline(file, line)) {
        messages.push_back(from_json(nlohmann::json::parse(line)));
    }
    return messages;
}
```

---

## 25.12 Testing

### Tool Tests

`tests/test_tools.cpp`:

```cpp
#include <gtest/gtest.h>
#include "../src/tools.hpp"

TEST(Tools, ReadFile) {
    // Create temp file
    std::ofstream temp("test.txt");
    temp << "Hello, world!";
    temp.close();
    
    // Test read
    auto result = tool::read_file("test.txt");
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, "Hello, world!");
    
    // Cleanup
    std::remove("test.txt");
}

TEST(Tools, WriteFile) {
    auto result = tool::write_file("test.txt", "Test content");
    ASSERT_TRUE(result.has_value());
    
    // Verify
    auto read_result = tool::read_file("test.txt");
    EXPECT_EQ(*read_result, "Test content");
    
    std::remove("test.txt");
}
```

---

## 25.13 Deployment

### Single Binary

```bash
# Build release
cmake -DCMAKE_BUILD_TYPE=Release ..
make

# Strip symbols (smaller)
strip mini-agent

# Package
tar czf mini-agent-linux-x64.tar.gz mini-agent README.md
```

### Installation

```bash
# User install
mkdir -p ~/.local/bin
cp mini-agent ~/.local/bin/
export PATH="$HOME/.local/bin:$PATH"

# System install
sudo cp mini-agent /usr/local/bin/
```

---

## 25.14 Real-World Improvements

### For Production Use

1. **Better TUI:**
   - Use maya or FTXUI
   - Syntax highlighting
   - Message history scrolling

2. **Async I/O:**
   - Background thread pool
   - Non-blocking tool execution

3. **Error Handling:**
   - Retry logic
   - Rate limiting
   - Graceful degradation

4. **Security:**
   - Sandbox tools
   - Whitelist paths
   - User confirmation

5. **Performance:**
   - LazyBytes for artifacts
   - Incremental rendering
   - Virtual scrolling

---

## 25.15 Summary

**What we built:**
- ✅ Chat with Claude
- ✅ Tool calling (read, write, shell)
- ✅ Streaming responses
- ✅ Simple TUI
- ✅ ~400 lines of code

**What we applied:**
- The Elm Architecture (pure update, effects as data)
- std::variant (Msg types)
- std::expected (error handling)
- RAII (file handles)
- Value semantics (model ownership)

**Key insight:** A minimal agent is **simple**. Production complexity comes from polish, not core logic.

---

## 25.16 Where to Go From Here

### Learn More

1. **Study agentty's codebase** (60K+ lines, production-ready)
2. **Read maya's source** (TUI framework)
3. **Experiment with providers** (OpenAI, Ollama, local models)
4. **Add RAG** (vector search for code)
5. **Build MCP servers** (custom tool backends)

### Resources

- [Anthropic API Docs](https://docs.anthropic.com/)
- [OpenAI API Docs](https://platform.openai.com/docs/)
- [agentty GitHub](https://github.com/1ay1/agentty)
- [C++23 Reference](https://en.cppreference.com/)

---

## Conclusion

**You've mastered modern C++:**
- Memory management (RAII, smart pointers, move semantics)
- Type safety (variants, expected, concepts)
- Performance (SIMD, zero-overhead, inlining)
- Architecture (Elm, effect systems, DI)
- Real-world systems (agentty, maya)

**Go build something amazing.**

---

**Previous:** [Chapter 24: Lazy Loading](../ch24-lazy-load/README.md)  
**Up:** [Part V: Case Studies](../README.md)  
**Home:** [Table of Contents](../../TABLE_OF_CONTENTS.md)
