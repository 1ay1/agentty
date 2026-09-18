# Chapter 7: std::expected and Monadic Error Handling

**Goal:** Master error handling without exceptions using std::expected and monadic composition.

**Time:** 4-5 hours  
**Prerequisites:** Chapters 1-6

---

## 7.1 Why Not Exceptions?

### The Problems with Exceptions

**1. Hidden control flow:**

```cpp
void process_file(const std::string& path) {
    auto data = read_file(path);  // Might throw!
    auto parsed = parse(data);     // Might throw!
    auto result = transform(parsed);  // Might throw!
    save(result);                  // Might throw!
}
```

**Questions you can't answer by reading the code:**
- Which functions throw?
- What exceptions can they throw?
- Are exceptions properly caught?

**2. Performance cost:**

```cpp
// Exception path (slow)
try {
    auto result = might_fail();
    return result;
} catch (const std::exception& e) {
    return handle_error(e);
}
```

**Why it's slow:**
- Stack unwinding
- Exception object allocation
- RTTI overhead
- Branch misprediction

**Measured cost** (from real benchmarks):
- Happy path: ~0 overhead (zero-cost exceptions)
- Error path: 10-100× slower than returning an error code

**3. Not compositional:**

```cpp
// Can't chain operations easily
auto result1 = operation1();  // Might throw
auto result2 = operation2(result1);  // Might throw  
auto result3 = operation3(result2);  // Might throw
```

Compare to:
```cpp
// With std::expected (preview)
return operation1()
    .and_then(operation2)
    .and_then(operation3);
```

**4. Binary size:**

Exception handling tables increase binary size:

```
# Without exceptions:
-fno-exceptions: 16.7 MB

# With exceptions:
(default): 17.4 MB (+700 KB just for exception tables)
```

### When Exceptions ARE Appropriate

Exceptions are good for:

1. **Constructor failures** — Can't return error code
2. **RAII failures** — Destructor can't propagate errors
3. **Truly exceptional conditions** — Out of memory, hardware failure
4. **Legacy C++ code** — Already using exceptions everywhere

**agentty uses exceptions for:**
- JSON parsing errors (nlohmann::json throws)
- Standard library failures (std::vector::at() throws)
- Assertion failures in debug builds

**agentty does NOT use exceptions for:**
- HTTP errors (404, 500)
- Parse failures (invalid tool arguments)
- File not found
- Network timeouts

**Why?** These are **expected failures**, not exceptional conditions.

---

## 7.2 std::expected<T, E> Basics

### What Is std::expected?

```cpp
#include <expected>

// Result type: either T (success) or E (error)
std::expected<int, std::string> divide(int a, int b) {
    if (b == 0) {
        return std::unexpected{"Division by zero"};
    }
    return a / b;
}

// Usage:
auto result = divide(10, 2);
if (result) {
    std::cout << "Success: " << *result;  // 5
} else {
    std::cout << "Error: " << result.error();
}
```

**Key properties:**

1. **Two states:** `T` (value) or `E` (error), never both
2. **Explicit checking:** Must check before accessing value
3. **Zero overhead:** Same size as `std::variant<T, E>`
4. **Compositional:** Supports monadic operations

### Construction

```cpp
// Success value
std::expected<int, std::string> success = 42;

// Error value
std::expected<int, std::string> error = std::unexpected{"failed"};

// From function
std::expected<int, std::string> result = divide(10, 0);
```

### Checking and Accessing

```cpp
std::expected<int, std::string> result = divide(10, 2);

// Method 1: Boolean conversion
if (result) {
    int value = *result;  // Dereference to get value
}

// Method 2: has_value()
if (result.has_value()) {
    int value = result.value();  // Throws if error (not recommended)
}

// Method 3: value_or()
int value = result.value_or(0);  // Returns 0 if error

// Method 4: Error access
if (!result) {
    std::string err = result.error();
}
```

### Real Example from maya: Terminal Initialization

```cpp
// maya/include/maya/core/expected.hpp

enum class ErrorKind : uint8_t {
    TerminalInit,
    Io,
    InvalidUtf8,
    // ...
};

struct Error {
    ErrorKind kind;
    std::string message;
    std::source_location location;
};

template <typename T>
using Result = std::expected<T, Error>;

// Usage in terminal initialization:
Result<Terminal> init_terminal() {
    #ifdef _WIN32
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    if (hOut == INVALID_HANDLE_VALUE) {
        return std::unexpected{Error{
            .kind = ErrorKind::TerminalInit,
            .message = "Failed to get stdout handle",
            .location = std::source_location::current()
        }};
    }
    #else
    if (!isatty(STDOUT_FILENO)) {
        return std::unexpected{Error{
            .kind = ErrorKind::TerminalInit,
            .message = "stdout is not a terminal",
            .location = std::source_location::current()
        }};
    }
    #endif
    
    return Terminal{/* ... */};
}

// Caller:
auto terminal = init_terminal();
if (!terminal) {
    std::cerr << "Error: " << terminal.error().message << '\n';
    return 1;
}
```

---

## 7.3 Monadic Operations: and_then, or_else, transform

### The Problem: Error Propagation

**Without monadic operations (verbose):**

```cpp
Result<int> step1();
Result<int> step2(int x);
Result<int> step3(int x);

Result<int> process() {
    auto r1 = step1();
    if (!r1) return std::unexpected{r1.error()};
    
    auto r2 = step2(*r1);
    if (!r2) return std::unexpected{r2.error()};
    
    auto r3 = step3(*r2);
    if (!r3) return std::unexpected{r3.error()};
    
    return r3;
}
```

**With monadic operations (elegant):**

```cpp
Result<int> process() {
    return step1()
        .and_then(step2)
        .and_then(step3);
}
```

### and_then: Chain Operations That Can Fail

**Signature:**
```cpp
template <typename F>
auto and_then(F&& func) -> /* result of func */;
```

**Behavior:**
- If `this` has a value, call `func` with the value
- If `this` has an error, short-circuit and return the error
- `func` must return `std::expected<U, E>` for some `U`

**Example:**

```cpp
Result<int> parse_int(std::string_view s) {
    try {
        return std::stoi(std::string{s});
    } catch (...) {
        return std::unexpected{Error{/* ... */}};
    }
}

Result<int> validate_positive(int x) {
    if (x > 0) return x;
    return std::unexpected{Error{/* ... */}};
}

Result<int> double_value(int x) {
    return x * 2;
}

// Chain them:
Result<int> process(std::string_view input) {
    return parse_int(input)
        .and_then(validate_positive)
        .and_then(double_value);
}

auto result = process("42");  // Success: 84
auto error = process("-5");   // Error: not positive
auto error2 = process("abc"); // Error: parse failed
```

**How it works:**

```
parse_int("42") → Result<int>{42}
  ↓ and_then(validate_positive)
validate_positive(42) → Result<int>{42}
  ↓ and_then(double_value)
double_value(42) → Result<int>{84}
  
Final: Result<int>{84}
```

```
parse_int("-5") → Result<int>{-5}
  ↓ and_then(validate_positive)
validate_positive(-5) → std::unexpected{Error{"not positive"}}
  ↓ and_then(double_value) — SHORT CIRCUIT
  
Final: std::unexpected{Error{"not positive"}}
```

### transform: Map Over Success Value

**Signature:**
```cpp
template <typename F>
auto transform(F&& func) -> std::expected</* result of func */, E>;
```

**Behavior:**
- If `this` has a value, call `func` with the value and wrap result
- If `this` has an error, short-circuit
- `func` returns a plain value (not `std::expected`)

**Example:**

```cpp
Result<int> get_age() {
    return 25;
}

Result<std::string> format_age() {
    return get_age()
        .transform([](int age) {
            return "Age: " + std::to_string(age);
        });
}

auto result = format_age();  // Result<std::string>{"Age: 25"}
```

**Difference from and_then:**

```cpp
// and_then: func returns Result<U>
.and_then([](int x) -> Result<int> {
    if (x > 0) return x * 2;
    return std::unexpected{Error{}};
})

// transform: func returns plain U
.transform([](int x) -> int {
    return x * 2;
})
```

### or_else: Handle Errors

**Signature:**
```cpp
template <typename F>
auto or_else(F&& func) -> std::expected<T, /* result of func */>;
```

**Behavior:**
- If `this` has a value, return it unchanged
- If `this` has an error, call `func` with the error
- `func` must return `std::expected<T, E2>` for some `E2`

**Example:**

```cpp
Result<int> try_primary_source();
Result<int> try_fallback_source(Error err);
Result<int> try_last_resort(Error err);

Result<int> get_value() {
    return try_primary_source()
        .or_else(try_fallback_source)
        .or_else(try_last_resort);
}
```

**Retry example:**

```cpp
Result<Response> fetch_with_retry(const Request& req) {
    return fetch(req)
        .or_else([&](Error err) {
            if (err.kind == ErrorKind::RateLimit) {
                std::this_thread::sleep_for(err.retry_after);
                return fetch(req);  // Retry once
            }
            return std::unexpected{err};  // Propagate other errors
        });
}
```

### Real Example from agentty: Provider Error Handling

```cpp
// include/agentty/provider/error_class.hpp

enum class ErrorClass {
    Transient,   // Retry immediately
    RateLimit,   // Retry after delay
    Auth,        // Re-authenticate
    Cancelled,   // User cancelled
    Terminal     // Don't retry
};

ErrorClass classify(const HttpError& err);

// src/runtime/app/update/stream.cpp
Cmd<Msg> handle_stream_error(Model& m, StreamError error) {
    ErrorClass cls = classify(error.http_error);
    
    return std::visit(overload{
        [&](Transient) -> Cmd<Msg> {
            // Retry immediately
            m.retry_count++;
            if (m.retry_count < 3) {
                return Cmd<Msg>::task([req = m.request] {
                    return stream_request(req);
                });
            }
            return Cmd<Msg>::none();  // Give up
        },
        
        [&](RateLimit) -> Cmd<Msg> {
            // Retry after delay
            auto delay = error.retry_after.value_or(std::chrono::seconds{60});
            return Cmd<Msg>::delay(delay, [req = m.request] {
                return stream_request(req);
            });
        },
        
        [&](Auth) -> Cmd<Msg> {
            // Re-authenticate
            return Cmd<Msg>::task([] {
                return LoginRequired{};
            });
        },
        
        [&](Cancelled) -> Cmd<Msg> {
            // User cancelled, do nothing
            return Cmd<Msg>::none();
        },
        
        [&](Terminal) -> Cmd<Msg> {
            // Surface to user
            m.error_message = error.message;
            return Cmd<Msg>::none();
        },
    }, cls);
}
```

---

## 7.4 Real Example: maya's Result<T>

### The Complete Implementation

```cpp
// maya/include/maya/core/expected.hpp

enum class ErrorKind : uint8_t {
    TerminalInit,
    Io,
    LayoutOverflow,
    InvalidStyle,
    InvalidUtf8,
    Unsupported,
    Signal,
    WouldBlock,
};

struct Error {
    ErrorKind kind;
    std::string message;
    std::source_location location = std::source_location::current();
    
    static Error io(std::string msg) {
        return Error{ErrorKind::Io, std::move(msg)};
    }
    
    static Error invalid_utf8(std::string msg) {
        return Error{ErrorKind::InvalidUtf8, std::move(msg)};
    }
    
    // ... more factory functions
};

template <typename T>
using Result = std::expected<T, Error>;

// Alias for void result (success with no value)
using Status = Result<void>;
```

### Usage Patterns

**1. Return success:**

```cpp
Result<int> parse_number(std::string_view s) {
    if (s.empty()) {
        return std::unexpected{Error::invalid_input("empty string")};
    }
    
    int value = 0;
    // ... parsing logic
    
    return value;
}
```

**2. Chain operations:**

```cpp
Result<Frame> render_frame(const Model& m) {
    return layout(m)
        .and_then([](Layout l) { return paint(l); })
        .and_then([](Canvas c) { return diff(c); })
        .and_then([](Diff d) { return encode(d); });
}
```

**3. Early return on error:**

```cpp
Result<void> initialize() {
    auto terminal = init_terminal();
    if (!terminal) {
        return std::unexpected{terminal.error()};
    }
    
    auto input = init_input(*terminal);
    if (!input) {
        return std::unexpected{input.error()};
    }
    
    return {};  // Success (void result)
}
```

**4. Use macros for ergonomics:**

```cpp
// maya/include/maya/core/expected.hpp

#define MAYA_TRY(decl, expr) \
    auto&& _result = (expr); \
    if (!_result) return std::unexpected{_result.error()}; \
    decl = std::move(*_result)

// Usage:
Result<int> complex_operation() {
    MAYA_TRY(auto value1, step1());
    MAYA_TRY(auto value2, step2(value1));
    MAYA_TRY(auto value3, step3(value2));
    return value3;
}
```

---

## 7.5 Error Classification in agentty

### The Error Hierarchy

```cpp
// include/agentty/provider/error_class.hpp

enum class ErrorClass {
    Transient,   // Network blip, retry immediately
    RateLimit,   // 429/529, retry after delay
    Auth,        // 401/403, re-authenticate
    Cancelled,   // User cancelled, no retry
    Terminal     // 400/404, don't retry
};

// Classify HTTP errors
ErrorClass classify(const HttpError& err) {
    switch (err.kind) {
        case HttpErrorKind::Cancelled:
            return ErrorClass::Cancelled;
            
        case HttpErrorKind::Status:
            if (err.http_status == 401 || err.http_status == 403) {
                return ErrorClass::Auth;
            }
            if (err.http_status == 429) {
                return ErrorClass::RateLimit;
            }
            if (err.http_status >= 500 && err.http_status < 600) {
                return ErrorClass::Transient;
            }
            return ErrorClass::Terminal;
            
        case HttpErrorKind::Tls:
        case HttpErrorKind::Dns:
        case HttpErrorKind::Connect:
        case HttpErrorKind::Timeout:
            return ErrorClass::Transient;
            
        case HttpErrorKind::Body:
            return ErrorClass::Terminal;
    }
}

// Classify string error messages (from SSE events)
ErrorClass classify(std::string_view message) {
    if (message.find("overloaded") != std::string_view::npos ||
        message.find("rate_limit") != std::string_view::npos) {
        return ErrorClass::RateLimit;
    }
    
    if (message.find("authentication") != std::string_view::npos ||
        message.find("invalid_api_key") != std::string_view::npos) {
        return ErrorClass::Auth;
    }
    
    return ErrorClass::Terminal;
}
```

### Smart Retry Logic

```cpp
// src/runtime/app/update/stream.cpp

struct RetryState {
    int count = 0;
    std::chrono::steady_clock::time_point last_attempt;
    std::chrono::seconds backoff{1};
};

Cmd<Msg> retry_or_fail(Model& m, const StreamError& err) {
    ErrorClass cls = classify(err.http_error);
    
    if (cls == ErrorClass::Cancelled || cls == ErrorClass::Terminal) {
        return surface_error(err);
    }
    
    if (cls == ErrorClass::Auth) {
        return Cmd<Msg>::task([] {
            return OpenLogin{};
        });
    }
    
    // Transient or RateLimit
    if (m.retry_state.count >= 3) {
        return surface_error(err);  // Give up after 3 tries
    }
    
    m.retry_state.count++;
    
    if (cls == ErrorClass::RateLimit && err.retry_after) {
        // Use server-provided retry-after
        return Cmd<Msg>::delay(*err.retry_after, [req = m.request] {
            return stream_request(req);
        });
    }
    
    // Exponential backoff for transient errors
    auto delay = m.retry_state.backoff;
    m.retry_state.backoff *= 2;
    m.retry_state.backoff = std::min(m.retry_state.backoff, std::chrono::seconds{60});
    
    return Cmd<Msg>::delay(delay, [req = m.request] {
        return stream_request(req);
    });
}
```

### Logging Errors with Context

```cpp
// src/util/logx.cpp

void log_error(const Error& err, std::string_view context) {
    std::ostringstream oss;
    oss << "[ERROR] " << context << ": " << err.message;
    
    // Include source location if available
    if (err.location.file_name()) {
        oss << " (at " << err.location.file_name() 
            << ":" << err.location.line() << ")";
    }
    
    // Include error kind
    oss << " [" << to_string(err.kind) << "]";
    
    AGT_LOG(Runtime, Error, "error", "{}", oss.str());
}

// Usage:
auto result = fetch_data();
if (!result) {
    log_error(result.error(), "fetch_data");
}
```

---

## 7.6 Exercises

### Exercise 7.1: Implement Safe Division

```cpp
Result<double> safe_divide(double a, double b) {
    // TODO: Return error if b == 0
}

// Test:
auto r1 = safe_divide(10.0, 2.0);
assert(r1 && *r1 == 5.0);

auto r2 = safe_divide(10.0, 0.0);
assert(!r2);
```

**Starter code:** `exercises/ch07/ex1-safe-divide.cpp`  
**Solution:** `solutions/ch07/ex1-safe-divide.cpp`

### Exercise 7.2: Parse and Validate

Chain operations using `and_then`:

```cpp
Result<int> parse_int(std::string_view s);
Result<int> validate_range(int x, int min, int max);
Result<int> double_value(int x);

Result<int> process(std::string_view input, int min, int max) {
    // TODO: Chain parse → validate → double using and_then
}

// Test:
auto r1 = process("42", 0, 100);
assert(r1 && *r1 == 84);

auto r2 = process("200", 0, 100);
assert(!r2);  // Out of range

auto r3 = process("abc", 0, 100);
assert(!r3);  // Parse error
```

**Starter code:** `exercises/ch07/ex2-chain.cpp`  
**Solution:** `solutions/ch07/ex2-chain.cpp`

### Exercise 7.3: File Operations

Implement file reading with error handling:

```cpp
Result<std::string> read_file(const std::filesystem::path& path);
Result<json> parse_json(const std::string& text);
Result<Config> validate_config(const json& j);

Result<Config> load_config(const std::filesystem::path& path) {
    // TODO: Chain read → parse → validate
}

// Test:
auto cfg = load_config("config.json");
if (cfg) {
    // Use *cfg
} else {
    std::cerr << cfg.error().message << '\n';
}
```

**Starter code:** `exercises/ch07/ex3-file-ops.cpp`  
**Solution:** `solutions/ch07/ex3-file-ops.cpp`

### Exercise 7.4: Retry Logic

Implement a retry wrapper:

```cpp
template <typename F>
Result<typename std::invoke_result_t<F>::value_type> 
retry(F func, int max_attempts, std::chrono::milliseconds delay) {
    // TODO: Call func up to max_attempts times
    // Sleep `delay` between attempts
    // Return first success or last error
}

// Test:
int attempt_count = 0;
auto func = [&]() -> Result<int> {
    attempt_count++;
    if (attempt_count < 3) {
        return std::unexpected{Error::io("failed")};
    }
    return 42;
};

auto result = retry(func, 5, std::chrono::milliseconds{10});
assert(result && *result == 42);
assert(attempt_count == 3);
```

**Starter code:** `exercises/ch07/ex4-retry.cpp`  
**Solution:** `solutions/ch07/ex4-retry.cpp`

### Exercise 7.5: Error Recovery

Implement fallback logic with `or_else`:

```cpp
Result<Data> fetch_from_cache(Key k);
Result<Data> fetch_from_db(Key k);
Result<Data> fetch_from_api(Key k);

Result<Data> fetch(Key k) {
    // TODO: Try cache → db → API using or_else
}

// Test:
// Should return cached data if available
// Otherwise try DB
// Otherwise try API
// Return error only if all three fail
```

**Starter code:** `exercises/ch07/ex5-recovery.cpp`  
**Solution:** `solutions/ch07/ex5-recovery.cpp`

---

## Key Takeaways

1. **Exceptions have hidden costs**
   - Control flow invisibility
   - Performance on error path
   - Binary size increase
   - Use for truly exceptional conditions only

2. **std::expected makes errors explicit**
   - `Result<T>` forces error checking
   - Errors are values, not exceptions
   - Zero overhead on happy path

3. **Monadic operations enable composition**
   - `and_then`: chain fallible operations
   - `transform`: map over success value
   - `or_else`: handle errors and retry

4. **Error classification guides retry logic**
   - Transient → retry immediately
   - RateLimit → retry after delay
   - Auth → re-authenticate
   - Terminal → surface to user

5. **Pattern: Result<T, Error> everywhere**
   - maya uses it for all I/O
   - agentty uses it for HTTP, parsing, tools
   - Replaces exceptions in application code

6. **std::source_location tracks error origins**
   - Automatic file/line capture
   - No runtime overhead
   - Essential for debugging

---

## Next Chapter

[Chapter 8: std::visit and Pattern Matching →](../ch08-visit/README.md)

In the next chapter, you'll learn:
- The Visitor pattern
- std::visit for exhaustive matching
- Nested variant dispatch
- Performance characteristics
- How agentty's update() processes 200+ message types
