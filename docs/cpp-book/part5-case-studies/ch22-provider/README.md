# Chapter 22: Provider Abstraction

**Learning Objectives:**
- Design pluggable provider systems
- Handle multiple API formats (Anthropic, OpenAI, Ollama)
- Implement streaming response parsing
- Use concepts for provider contracts
- See agentty's multi-provider architecture
- Support local and cloud models

---

## 22.1 The Problem

### Multiple AI Providers

agentty supports:
- **Anthropic** (Claude models)
- **OpenAI** (GPT models)
- **Ollama** (local models)

**Each has:**
- Different API format
- Different auth
- Different streaming protocol
- Different capabilities

**Challenge:** Unified interface without losing flexibility.

---

## 22.2 The Provider Concept

### Interface Contract

`agentty/include/agentty/provider/concept.hpp`:

```cpp
template<typename P>
concept Provider = requires(P p, const Request& req, EventSink sink) {
    // Stream a request
    { p.stream(req, sink) } -> std::same_as<StreamResult>;
    
    // Provider metadata
    { p.name() } -> std::convertible_to<std::string_view>;
    { p.capabilities() } -> std::same_as<ProviderCapabilities>;
};

struct ProviderCapabilities {
    bool supports_images;
    bool supports_tools;
    bool supports_streaming;
    size_t max_tokens;
};
```

---

## 22.3 Anthropic Provider

### API Format

Anthropic uses **server-sent events** (SSE):

```
POST /v1/messages
Content-Type: application/json

{
  "model": "claude-3-5-sonnet-20241022",
  "messages": [{"role": "user", "content": "Hello"}],
  "stream": true
}
```

**Response:**

```
data: {"type":"message_start","message":{"id":"msg_123"}}
data: {"type":"content_block_delta","delta":{"type":"text","text":"Hello"}}
data: {"type":"message_stop"}
```

### Implementation

`agentty/src/provider/anthropic.cpp`:

```cpp
class AnthropicProvider {
    http::Client& http_;
    std::string api_key_;
    
public:
    StreamResult stream(const Request& req, EventSink sink) {
        // Build request
        auto body = nlohmann::json{
            {"model", req.model},
            {"messages", serialize_messages(req.messages)},
            {"stream", true},
            {"max_tokens", req.max_tokens}
        };
        
        if (req.tools) {
            body["tools"] = serialize_tools(*req.tools);
        }
        
        // Send HTTP request
        auto response = http_.post(
            "https://api.anthropic.com/v1/messages",
            body.dump(),
            {{"x-api-key", api_key_}}
        );
        
        // Parse SSE stream
        SSEParser parser;
        for (auto chunk : response.stream()) {
            for (auto event : parser.parse(chunk)) {
                handle_event(event, sink);
            }
        }
        
        return StreamResult::ok();
    }
    
    std::string_view name() const { return "anthropic"; }
    
    ProviderCapabilities capabilities() const {
        return {
            .supports_images = true,
            .supports_tools = true,
            .supports_streaming = true,
            .max_tokens = 200'000
        };
    }
    
private:
    void handle_event(const SSEEvent& event, EventSink& sink) {
        auto j = nlohmann::json::parse(event.data);
        auto type = j["type"].get<std::string>();
        
        if (type == "content_block_delta") {
            auto delta = j["delta"]["text"].get<std::string>();
            sink(StreamTextDelta{delta});
        }
        else if (type == "tool_use") {
            auto name = j["name"].get<std::string>();
            auto input = j["input"];
            sink(StreamToolCall{name, input});
        }
        else if (type == "message_stop") {
            sink(StreamEnd{});
        }
    }
};
```

---

## 22.4 OpenAI Provider

### API Format

OpenAI uses **chunked responses**:

```
POST /v1/chat/completions
Content-Type: application/json

{
  "model": "gpt-4",
  "messages": [{"role": "user", "content": "Hello"}],
  "stream": true
}
```

**Response:**

```
data: {"choices":[{"delta":{"content":"Hello"}}]}
data: {"choices":[{"delta":{"content":" there"}}]}
data: [DONE]
```

### Implementation

`agentty/src/provider/openai.cpp`:

```cpp
class OpenAIProvider {
    http::Client& http_;
    std::string api_key_;
    
public:
    StreamResult stream(const Request& req, EventSink sink) {
        auto body = nlohmann::json{
            {"model", req.model},
            {"messages", serialize_messages(req.messages)},
            {"stream", true}
        };
        
        auto response = http_.post(
            "https://api.openai.com/v1/chat/completions",
            body.dump(),
            {{"Authorization", "Bearer " + api_key_}}
        );
        
        for (auto chunk : response.stream()) {
            if (chunk == "data: [DONE]\n") break;
            
            if (chunk.starts_with("data: ")) {
                auto j = nlohmann::json::parse(chunk.substr(6));
                auto delta = j["choices"][0]["delta"];
                
                if (delta.contains("content")) {
                    sink(StreamTextDelta{delta["content"]});
                }
            }
        }
        
        return StreamResult::ok();
    }
    
    std::string_view name() const { return "openai"; }
    
    ProviderCapabilities capabilities() const {
        return {
            .supports_images = true,
            .supports_tools = true,
            .supports_streaming = true,
            .max_tokens = 128'000
        };
    }
};
```

---

## 22.5 Ollama Provider (Local)

### API Format

Ollama runs **locally**:

```
POST http://localhost:11434/api/chat
Content-Type: application/json

{
  "model": "llama3.2",
  "messages": [{"role": "user", "content": "Hello"}],
  "stream": true
}
```

**Response:**

```
{"message":{"content":"Hello"}}
{"message":{"content":" there"}}
{"done":true}
```

### Implementation

`agentty/src/provider/ollama.cpp`:

```cpp
class OllamaProvider {
    std::string base_url_ = "http://localhost:11434";
    
public:
    StreamResult stream(const Request& req, EventSink sink) {
        auto body = nlohmann::json{
            {"model", req.model},
            {"messages", serialize_messages(req.messages)},
            {"stream", true}
        };
        
        auto response = http_.post(
            base_url_ + "/api/chat",
            body.dump()
        );
        
        for (auto line : response.lines()) {
            auto j = nlohmann::json::parse(line);
            
            if (j.contains("message")) {
                auto content = j["message"]["content"].get<std::string>();
                sink(StreamTextDelta{content});
            }
            
            if (j.value("done", false)) {
                sink(StreamEnd{});
            }
        }
        
        return StreamResult::ok();
    }
    
    std::string_view name() const { return "ollama"; }
    
    ProviderCapabilities capabilities() const {
        return {
            .supports_images = false,  // Model-dependent
            .supports_tools = false,   // Not yet
            .supports_streaming = true,
            .max_tokens = 8192  // Model-dependent
        };
    }
};
```

---

## 22.6 Provider Registry

### Dynamic Selection

`agentty/include/agentty/provider/registry.hpp`:

```cpp
class ProviderRegistry {
    std::unordered_map<std::string, std::function<std::unique_ptr<Provider>()>> factories_;
    
public:
    template<typename P>
    void register_provider(std::string name, std::function<std::unique_ptr<P>()> factory) {
        static_assert(Provider<P>, "P must satisfy Provider concept");
        factories_[name] = [factory]() -> std::unique_ptr<Provider> {
            return factory();
        };
    }
    
    std::unique_ptr<Provider> get(std::string name) {
        if (auto it = factories_.find(name); it != factories_.end()) {
            return it->second();
        }
        return nullptr;
    }
    
    std::vector<std::string> list() const {
        std::vector<std::string> names;
        for (auto& [name, _] : factories_) {
            names.push_back(name);
        }
        return names;
    }
};
```

### Registration at Startup

```cpp
void register_providers(ProviderRegistry& registry) {
    registry.register_provider("anthropic", []() {
        return std::make_unique<AnthropicProvider>(get_api_key("anthropic"));
    });
    
    registry.register_provider("openai", []() {
        return std::make_unique<OpenAIProvider>(get_api_key("openai"));
    });
    
    registry.register_provider("ollama", []() {
        return std::make_unique<OllamaProvider>();
    });
}
```

---

## 22.7 Streaming Response Parser

### The Challenge

Streaming responses arrive in **chunks**:

```
"data: {\"type\":\"content"
"_block_delta\",\"delta\":{\"tex"
"t\":\"Hello\"}}\n"
```

**Problem:** JSON spans multiple chunks.

### Solution: Buffered Parser

`agentty/src/provider/sse_parser.cpp`:

```cpp
class SSEParser {
    std::string buffer_;
    
public:
    std::vector<SSEEvent> parse(std::string_view chunk) {
        buffer_ += chunk;
        std::vector<SSEEvent> events;
        
        // Split by double newline (event boundary)
        size_t pos = 0;
        while ((pos = buffer_.find("\n\n")) != std::string::npos) {
            auto event_text = buffer_.substr(0, pos);
            buffer_.erase(0, pos + 2);
            
            // Parse event
            if (event_text.starts_with("data: ")) {
                events.push_back(SSEEvent{
                    .data = event_text.substr(6)
                });
            }
        }
        
        return events;
    }
};
```

---

## 22.8 Error Handling

### StreamResult

`agentty/include/agentty/provider/stream_result.hpp`:

```cpp
struct StreamResult {
    enum class Status {
        Ok,
        Cancelled,
        RateLimited,
        ServerError,
        NetworkError
    };
    
    Status status;
    std::optional<std::string> error_message;
    std::optional<int> retry_after_seconds;
    
    static StreamResult ok() {
        return {Status::Ok, std::nullopt, std::nullopt};
    }
    
    static StreamResult failed(std::string msg) {
        return {Status::ServerError, std::move(msg), std::nullopt};
    }
    
    static StreamResult rate_limited(int retry_after) {
        return {Status::RateLimited, "Rate limited", retry_after};
    }
    
    bool is_ok() const { return status == Status::Ok; }
    bool should_retry() const { return status == Status::RateLimited; }
};
```

### Handling Errors

```cpp
auto result = provider.stream(req, sink);

if (!result.is_ok()) {
    if (result.should_retry()) {
        auto delay = result.retry_after_seconds.value_or(60);
        schedule_retry(req, delay);
    } else {
        show_error(result.error_message.value_or("Unknown error"));
    }
}
```

---

## 22.9 Testing Providers

### Fake Provider

`agentty/tests/fake_provider.cpp`:

```cpp
class FakeProvider {
    std::vector<std::string> canned_responses_;
    size_t call_count_ = 0;
    
public:
    FakeProvider(std::vector<std::string> responses)
        : canned_responses_(std::move(responses)) {}
    
    StreamResult stream(const Request& req, EventSink sink) {
        auto response = canned_responses_[call_count_++];
        
        // Simulate streaming
        for (char c : response) {
            sink(StreamTextDelta{std::string(1, c)});
        }
        sink(StreamEnd{});
        
        return StreamResult::ok();
    }
    
    std::string_view name() const { return "fake"; }
    ProviderCapabilities capabilities() const { return {}; }
};
```

### Test

```cpp
TEST(Provider, StreamsResponse) {
    FakeProvider provider{{"Hello, world!"}};
    
    std::string accumulated;
    auto sink = [&](const StreamEvent& event) {
        std::visit(overload{
            [&](const StreamTextDelta& d) { accumulated += d.delta; },
            [](const StreamEnd&) {}
        }, event);
    };
    
    auto result = provider.stream(Request{}, sink);
    
    EXPECT_TRUE(result.is_ok());
    EXPECT_EQ(accumulated, "Hello, world!");
}
```

---

## 22.10 Summary

**What we learned:**
- ✅ Provider concept defines the contract
- ✅ Each provider handles its API format
- ✅ Registry enables dynamic selection
- ✅ Streaming parsers handle chunked responses
- ✅ StreamResult unifies error handling
- ✅ Fake providers enable testing

**Key insight:** Use **concepts** for compile-time contracts, **type erasure** for runtime polymorphism.

---

## 22.11 Exercises

### Exercise 1: Add Provider

Implement a `GroqProvider` for the Groq API.

---

### Exercise 2: Streaming Parser

Write a parser for OpenAI's chunked format.

---

### Exercise 3: Retry Logic

Implement exponential backoff for rate-limited requests.

---

## Next Chapter

In [Chapter 24: Lazy Loading and LazyBytes](../ch24-lazy-load/README.md), we'll optimize memory usage with deferred loading.

---

**Previous:** [Chapter 21: Thread Persistence](../ch21-thread-persist/README.md)  
**Next:** [Chapter 24: Lazy Loading](../ch24-lazy-load/README.md)  
**Up:** [Part V: Case Studies](../README.md)
