// 10_imagecontent.cpp — a faithful rebuild of agentty's ImageContent.
//
// Everything in chapter 1 shows up here at once: a strong id, explicit,
// noexcept, [[nodiscard]], a mutable lazy cache, const correctness,
// move-only payload handling, and designated initialisers.
//
// Compare with include/agentty/domain/conversation.hpp (ImageContent) and
// include/agentty/domain/lazy_bytes.hpp when you're done.
//
// Build: make 10_imagecontent && ./10_imagecontent

#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <utility>
#include <vector>

// ── the strong id, from section 3 ──────────────────────────────────────
template <typename Tag>
struct Id {
    std::string value;
    Id() = default;
    explicit Id(std::string s) noexcept : value(std::move(s)) {}
    [[nodiscard]] bool        empty() const noexcept { return value.empty(); }
    [[nodiscard]] const char* c_str() const noexcept { return value.c_str(); }
    bool operator==(const Id&) const = default;
};
struct MessageIdTag {};
using MessageId = Id<MessageIdTag>;

// ── the lazy payload, from section 6 ───────────────────────────────────
class LazyBytes {
public:
    struct Source {
        std::string blob;   // name in the content-addressed store
        std::string b64;    // or inline base64 from an old thread file
        [[nodiscard]] bool empty() const noexcept {
            return blob.empty() && b64.empty();
        }
    };

    LazyBytes() = default;

    // eager: a paste or a screenshot, bytes already in hand
    explicit LazyBytes(std::string bytes) noexcept
        : bytes_(std::move(bytes)), resolved_(true) {}

    // lazy: loaded from disk, nothing decoded yet
    explicit LazyBytes(Source src) noexcept
        : source_(std::move(src)), resolved_(false) {}

    // const, because resolving is not a logical change.
    [[nodiscard]] const std::string& bytes() const {
        if (!resolved_) {
            std::printf("    [resolve blob=%s b64=%zub]\n",
                        source_.blob.empty() ? "-" : source_.blob.c_str(),
                        source_.b64.size());
            bytes_    = resolve(source_);
            resolved_ = true;
        }
        return bytes_;
    }

    // cheap questions that must NOT force materialisation.
    [[nodiscard]] bool empty() const noexcept {
        return resolved_ ? bytes_.empty() : source_.empty();
    }
    [[nodiscard]] bool materialised() const noexcept { return resolved_; }

    using Resolver = std::string (*)(const Source&);
    static void set_resolver(Resolver r) noexcept { resolver_ = r; }

private:
    static std::string resolve(const Source& s) {
        if (resolver_) return resolver_(s);
        return {};                       // null resolver == missing blob
    }

    inline static Resolver resolver_ = nullptr;

    Source              source_;
    mutable std::string bytes_;
    mutable bool        resolved_ = true;
};

// ── the image, from all of it ──────────────────────────────────────────
class ImageContent {
public:
    using Source = LazyBytes::Source;

    std::string media_type;            // "image/png", "image/jpeg", ...
    std::uint32_t width  = 0;
    std::uint32_t height = 0;

    ImageContent() = default;

    ImageContent(std::string mt, std::string raw) noexcept
        : media_type(std::move(mt)), data_(std::move(raw)) {}

    ImageContent(std::string mt, Source src) noexcept
        : media_type(std::move(mt)), data_(std::move(src)) {}

    [[nodiscard]] const std::string& bytes() const { return data_.bytes(); }
    [[nodiscard]] bool empty() const noexcept { return data_.empty(); }
    [[nodiscard]] bool materialised() const noexcept { return data_.materialised(); }

private:
    LazyBytes data_;
};

// ── a message that owns some of these ──────────────────────────────────
struct Message {
    MessageId                 id;
    std::string               text;
    std::vector<ImageContent> images;
};

// ── a fake blob store so the demo actually resolves something ──────────
static std::string fake_store(const LazyBytes::Source& s) {
    if (s.blob == "sha256-aa11") return std::string(4096, '\x89');
    if (!s.b64.empty())          return "decoded(" + s.b64 + ")";
    return {};
}

int main() {
    LazyBytes::set_resolver(&fake_store);

    std::puts("-- eager image (a paste): bytes are already here --");
    ImageContent pasted{"image/png", std::string(1024, '\x89')};
    pasted.width = 64;
    pasted.height = 64;
    std::printf("  materialised? %s\n", pasted.materialised() ? "yes" : "no");
    std::printf("  empty?        %s\n", pasted.empty() ? "yes" : "no");
    std::printf("  bytes:        %zu\n", pasted.bytes().size());

    std::puts("\n-- lazy image (loaded from a thread file) --");
    ImageContent loaded{"image/png", ImageContent::Source{.blob = "sha256-aa11", .b64 = {}}};
    std::printf("  materialised? %s   <- nothing decoded yet\n",
                loaded.materialised() ? "yes" : "no");
    std::printf("  empty?        %s   <- answered WITHOUT resolving\n",
                loaded.empty() ? "yes" : "no");
    std::puts("  now ask for bytes:");
    std::printf("  bytes:        %zu\n", loaded.bytes().size());
    std::printf("  ask again:    %zu   <- no second resolve\n",
                loaded.bytes().size());

    std::puts("\n-- a missing blob resolves to empty, not a crash --");
    ImageContent gone{"image/png", ImageContent::Source{.blob = "sha256-dead", .b64 = {}}};
    std::printf("  bytes: %zu\n", gone.bytes().size());

    std::puts("\n-- the whole thing inside a message --");
    Message m{
        .id     = MessageId{"msg-01"},
        .text   = "look at this",
        .images = {},
    };
    m.images.push_back(std::move(pasted));
    m.images.emplace_back("image/jpeg",
                          ImageContent::Source{.blob = {}, .b64 = "SGVsbG8="});

    std::printf("  message %s: '%s', %zu image(s)\n",
                m.id.c_str(), m.text.c_str(), m.images.size());
    for (const auto& img : m.images)
        std::printf("    %-10s resolved=%-3s bytes=%zu\n",
                    img.media_type.c_str(),
                    img.materialised() ? "yes" : "no",
                    img.bytes().size());

    std::puts("\n-- const message: you can still read the bytes --");
    const Message& cm = m;
    std::printf("  first image bytes through a const& : %zu\n",
                cm.images.front().bytes().size());
    std::puts("  that call worked because bytes() is const and the cache is");
    std::puts("  mutable. that one design choice is the reason a loaded thread");
    std::puts("  opens without decoding megabytes nobody looks at.");
}
