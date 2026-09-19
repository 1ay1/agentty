# Chapter 24: Lazy Loading and LazyBytes

**Learning Objectives:**
- Implement lazy evaluation in C++
- Use std::optional for deferred computation
- Design LazyBytes for blob handling
- Optimize memory with on-demand loading
- See agentty's lazy image loading
- Measure memory savings

---

## 24.1 The Problem

### Eager Loading

```cpp
struct Message {
    std::string text;
    std::vector<uint8_t> image_data;  // Always loaded
};

std::vector<Message> load_thread(ThreadId id) {
    std::vector<Message> messages;
    for (auto& msg_json : read_jsonl(id)) {
        Message msg;
        msg.text = msg_json["text"];
        
        // Load image even if never displayed
        if (msg_json.contains("image_blob")) {
            auto hash = msg_json["image_blob"];
            msg.image_data = load_blob(hash);  // 2 MB
        }
        
        messages.push_back(std::move(msg));
    }
    return messages;
}
```

**Problem:**
- 26K messages with images = **52 GB** memory
- Most images never displayed (scrolled past)
- Startup takes **90 seconds**

---

## 24.2 Lazy Evaluation

### The Pattern

Don't compute until needed:

```cpp
template<typename T>
class Lazy {
    mutable std::optional<T> value_;
    std::function<T()> compute_;
    
public:
    Lazy(std::function<T()> compute) : compute_(std::move(compute)) {}
    
    const T& get() const {
        if (!value_) {
            value_ = compute_();  // Compute on first access
        }
        return *value_;
    }
};
```

**Usage:**

```cpp
Lazy<int> expensive_value{[]() {
    // Expensive computation
    return compute_something();
}};

// Not computed yet
std::cout << "Created lazy value\n";

// Now it's computed
std::cout << expensive_value.get();
```

---

## 24.3 LazyBytes

### The Type

`agentty/include/agentty/core/lazy_bytes.hpp`:

```cpp
class LazyBytes {
    struct Source {
        std::string blob_hash;
        size_t size;
    };
    
    mutable std::optional<std::vector<uint8_t>> materialized_;
    std::optional<Source> source_;
    
public:
    // Construct from source (not loaded yet)
    static LazyBytes from_blob(std::string hash, size_t size) {
        LazyBytes lb;
        lb.source_ = Source{std::move(hash), size};
        return lb;
    }
    
    // Construct from already-loaded bytes
    static LazyBytes from_bytes(std::vector<uint8_t> bytes) {
        LazyBytes lb;
        lb.materialized_ = std::move(bytes);
        return lb;
    }
    
    // Get bytes (load if needed)
    const std::vector<uint8_t>& bytes() const {
        if (!materialized_) {
            // Load from blob store
            materialized_ = load_blob(source_->blob_hash);
        }
        return *materialized_;
    }
    
    // Check if loaded
    bool is_materialized() const {
        return materialized_.has_value();
    }
    
    size_t size() const {
        if (materialized_) {
            return materialized_->size();
        }
        return source_->size;
    }
};
```

---

## 24.4 Using LazyBytes in Messages

### The Message Type

```cpp
struct ImageMessage {
    std::string alt_text;
    LazyBytes image;  // Not loaded until displayed
};

struct Message {
    MessageId id;
    std::string text;
    std::optional<ImageMessage> image;
};
```

### Loading a Thread

```cpp
std::vector<Message> load_thread(ThreadId id) {
    std::vector<Message> messages;
    
    for (auto& msg_json : read_jsonl(id)) {
        Message msg;
        msg.text = msg_json["text"];
        
        if (msg_json.contains("image_blob")) {
            auto hash = msg_json["image_blob"];
            auto size = msg_json["image_size"];
            
            // Don't load yet — just remember where it is
            msg.image = ImageMessage{
                .alt_text = msg_json["alt_text"],
                .image = LazyBytes::from_blob(hash, size)
            };
        }
        
        messages.push_back(std::move(msg));
    }
    
    return messages;
}
```

**Result:**
- Load 26K messages: **1.4ms** (vs 90 seconds)
- Memory: **48 MB** (vs 52 GB)

---

## 24.5 Rendering with LazyBytes

### The Rendering Path

```cpp
void render_message(const Message& msg, Canvas& canvas) {
    // Render text (always loaded)
    canvas.draw_text(msg.text);
    
    // Render image (loaded only if visible)
    if (msg.image && is_visible(msg)) {
        // This triggers lazy load
        auto& image_data = msg.image->image.bytes();
        canvas.draw_image(image_data);
    }
}
```

**Lazy loading happens here:**
- User scrolls to message → `is_visible()` = true
- First render → `bytes()` loads from disk
- Subsequent renders → already loaded, instant

---

## 24.6 Preloading

### The Problem

Lazy loading on scroll causes **stuttering**:

- User scrolls
- Image not loaded
- Disk I/O (10ms)
- Frame drops

### Solution: Preload Ahead

```cpp
void preload_visible_images(const std::vector<Message>& messages, 
                             size_t first_visible, 
                             size_t last_visible) {
    // Preload current viewport
    for (size_t i = first_visible; i <= last_visible; ++i) {
        if (messages[i].image) {
            // Trigger load (async)
            load_async(messages[i].image->image);
        }
    }
    
    // Preload next 10 messages (ahead of scroll)
    for (size_t i = last_visible + 1; i <= last_visible + 10; ++i) {
        if (i < messages.size() && messages[i].image) {
            load_async(messages[i].image->image);
        }
    }
}
```

**Result:**
- Smooth scrolling (no stutters)
- Memory usage stays low (only viewport + ahead)

---

## 24.7 Async Loading

### The Implementation

`agentty/src/io/lazy_bytes_loader.cpp`:

```cpp
class LazyBytesLoader {
    std::thread worker_;
    std::queue<LazyBytes*> load_queue_;
    std::mutex mutex_;
    std::condition_variable cv_;
    bool running_ = true;
    
public:
    LazyBytesLoader() {
        worker_ = std::thread([this]() {
            while (running_) {
                std::unique_lock lock(mutex_);
                cv_.wait(lock, [this]() {
                    return !load_queue_.empty() || !running_;
                });
                
                if (!running_) break;
                
                auto* lazy = load_queue_.front();
                load_queue_.pop();
                lock.unlock();
                
                // Load in background
                lazy->bytes();  // Triggers load
            }
        });
    }
    
    void enqueue(LazyBytes& lazy) {
        if (lazy.is_materialized()) return;
        
        std::lock_guard lock(mutex_);
        load_queue_.push(&lazy);
        cv_.notify_one();
    }
};
```

---

## 24.8 Memory Management

### Eviction Policy

Can't keep all images in memory. Need to **evict** old ones:

```cpp
class LRUCache {
    struct Entry {
        std::string hash;
        std::vector<uint8_t> data;
        size_t last_access;
    };
    
    std::unordered_map<std::string, Entry> cache_;
    size_t max_size_ = 100 * 1024 * 1024;  // 100 MB
    size_t current_size_ = 0;
    size_t access_counter_ = 0;
    
public:
    std::vector<uint8_t> get(const std::string& hash) {
        if (auto it = cache_.find(hash); it != cache_.end()) {
            it->second.last_access = access_counter_++;
            return it->second.data;
        }
        
        // Load from disk
        auto data = load_blob(hash);
        
        // Evict if needed
        while (current_size_ + data.size() > max_size_) {
            evict_lru();
        }
        
        // Cache it
        cache_[hash] = Entry{hash, data, access_counter_++};
        current_size_ += data.size();
        
        return data;
    }
    
private:
    void evict_lru() {
        auto oldest = std::min_element(cache_.begin(), cache_.end(),
            [](auto& a, auto& b) {
                return a.second.last_access < b.second.last_access;
            });
        
        current_size_ -= oldest->second.data.size();
        cache_.erase(oldest);
    }
};
```

---

## 24.9 Measuring Impact

### Before Lazy Loading

```
Thread load time: 90 seconds
Memory usage: 52 GB
Startup: unusable
```

### After Lazy Loading

```
Thread load time: 1.4ms
Memory usage: 48 MB (metadata only)
Viewport load: 120ms (10 images)
Peak memory: 250 MB (with cache)
```

**Speedup:** 64,000× faster startup

---

## 24.10 Lazy Computation (Not Just Data)

### Lazy Values

```cpp
template<typename T>
class Lazy {
    mutable std::optional<T> value_;
    std::function<T()> compute_;
    
public:
    Lazy(std::function<T()> f) : compute_(std::move(f)) {}
    
    const T& operator*() const {
        if (!value_) value_ = compute_();
        return *value_;
    }
};
```

**Usage:**

```cpp
Lazy<int> fib_50{[]() { return fibonacci(50); }};

// Not computed yet
std::cout << "Defined lazy fib(50)\n";

// Computed on first use
std::cout << *fib_50;  // 12586269025

// Cached on second use
std::cout << *fib_50;  // Instant
```

---

## 24.11 Thread Safety

### The Problem

Multiple threads accessing LazyBytes:

```cpp
// Thread 1
auto& data1 = lazy.bytes();

// Thread 2 (same lazy)
auto& data2 = lazy.bytes();  // Race condition!
```

### Solution: Mutex

```cpp
class LazyBytes {
    mutable std::optional<std::vector<uint8_t>> materialized_;
    mutable std::mutex mutex_;
    
public:
    const std::vector<uint8_t>& bytes() const {
        std::lock_guard lock(mutex_);
        if (!materialized_) {
            materialized_ = load_blob(source_->blob_hash);
        }
        return *materialized_;
    }
};
```

**Cost:** One lock per lazy load (negligible).

---

## 24.12 Summary

**What we learned:**
- ✅ Lazy evaluation defers expensive work
- ✅ LazyBytes loads blobs on demand
- ✅ Preloading prevents stutter
- ✅ LRU cache limits memory usage
- ✅ 64,000× faster thread loading

**Key insight:** Don't load what you don't need. Load ahead of what you will.

---

## 24.13 Exercises

### Exercise 1: Implement Lazy<T>

```cpp
template<typename T>
class Lazy {
    // YOUR CODE
};

Lazy<int> x{[]() { return expensive_compute(); }};
std::cout << *x;  // Computed here
std::cout << *x;  // Cached
```

---

### Exercise 2: LRU Cache

Implement an LRU cache with max size 100 MB.

---

### Exercise 3: Measure Lazy Loading

Compare:
- Eager load 1000 images
- Lazy load 1000 images, display 10

Report time and memory.

---

## Next Chapter

In [Chapter 25: Building Your Own Terminal Agent](../ch25-build-agent/README.md), we'll wrap up by building a minimal AI agent from scratch.

---

**Previous:** [Chapter 22: Provider Abstraction](../ch22-provider/README.md)  
**Next:** [Chapter 25: Building Your Own Agent](../ch25-build-agent/README.md)  
**Up:** [Part V: Case Studies](../README.md)
