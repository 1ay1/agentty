#pragma once
// agentty::LazyBytes — a payload that knows how to fetch itself.
//
// Two things a Message carries are large, opaque, and read by almost
// nothing: image bytes and attachment bodies. Both live in the
// content-addressed blob store, both were historically base64 inline, and
// both are read on exactly one path — the wire, when the message is
// re-sent. The renderer never touches either: an image draws from its
// media type and dimensions, an attachment from its name and byte count.
//
// Decoding them at load time is therefore pure waste, and it is the
// expensive kind. Measured on a real store: one 29 MB thread carried
// 17 MB of image payload, and 26 captured-output attachments held 4.2 MB
// of base64 — all decoded on every open to produce bytes nobody looked at.
//
// So a loaded payload starts as a `Source` (a blob name, or base64 text)
// and materialises on the first `bytes()` call. Freshly captured payloads
// (a paste, a screenshot, a tool result) are constructed with their bytes
// present and never touch the lazy path.
//
// CORRECTNESS, because "lazy" is where bugs hide:
//   - bytes() is the ONLY reader. There is no public raw field, so no
//     caller can accidentally observe an unmaterialised payload as empty.
//   - Materialisation is idempotent and value-preserving: the same bytes
//     on every call, and a COPY copies the source, so the copy resolves
//     to the same bytes rather than to nothing.
//   - A source that fails to resolve (deleted blob, corrupt base64)
//     yields empty bytes — exactly what the eager loader produced for the
//     same input, and every consumer already handles an empty payload.
//   - `mutable` cache + const bytes(): materialising is not a logical
//     mutation, so it stays available on a const Message.
//
// WHY ITS OWN HEADER: both conversation.hpp (ImageContent) and
// composer_attachment.hpp (Attachment) need it, and conversation.hpp
// already includes composer_attachment.hpp — defining it in either would
// be a cycle. It is also genuinely general: it knows nothing about images
// or attachments, only about bytes that may not be here yet.
//
// The resolver is a function pointer installed once by the persistence
// layer, so this header stays free of any knowledge of the blob directory
// or of base64.

#include <string>
#include <utility>

namespace agentty {

class LazyBytes {
public:
    // How to obtain the bytes when first needed. An empty `blob` means the
    // payload is inline base64 in `b64`.
    struct Source {
        std::string blob;   // content-addressed blob name (preferred)
        std::string b64;    // legacy inline base64 fallback
        [[nodiscard]] bool empty() const noexcept {
            return blob.empty() && b64.empty();
        }
    };

    LazyBytes() = default;
    explicit LazyBytes(std::string raw)
        : bytes_(std::move(raw)), resolved_(true) {}

    [[nodiscard]] static LazyBytes lazy(Source src) {
        LazyBytes v;
        v.source_   = std::move(src);
        v.resolved_ = v.source_.empty();   // nothing to resolve => done
        return v;
    }
    [[nodiscard]] static LazyBytes from_blob(std::string name) {
        return lazy(Source{.blob = std::move(name), .b64 = {}});
    }
    [[nodiscard]] static LazyBytes from_base64(std::string b64) {
        return lazy(Source{.blob = {}, .b64 = std::move(b64)});
    }

    // The raw bytes, materialising them on first use.
    [[nodiscard]] const std::string& bytes() const {
        if (!resolved_) {
            bytes_    = resolver_ ? resolver_(source_) : std::string{};
            resolved_ = true;
        }
        return bytes_;
    }

    // Replace the payload outright (clipboard capture, tool result, an
    // @file read at submit time).
    void set_bytes(std::string raw) {
        bytes_    = std::move(raw);
        source_   = {};
        resolved_ = true;
    }

    // The pending source, for the writer: a payload that was never
    // materialised can be re-persisted BY REFERENCE, with no decode and no
    // re-encode. Empty once the bytes have been materialised or set.
    [[nodiscard]] const Source& source() const noexcept { return source_; }
    [[nodiscard]] bool materialised() const noexcept { return resolved_; }

    // "Is there nothing here?" without forcing a blob read: an
    // unmaterialised payload with a source is not empty, whatever the
    // source resolves to.
    [[nodiscard]] bool empty() const noexcept {
        return resolved_ ? bytes_.empty() : source_.empty();
    }

    // Installed once at startup by the persistence layer. A null resolver
    // (a unit test that never loads from disk) makes every lazy payload
    // resolve to empty, which is the same as a missing blob.
    using Resolver = std::string (*)(const Source&);
    static void set_resolver(Resolver r) noexcept { resolver_ = r; }

private:
    inline static Resolver resolver_ = nullptr;

    Source              source_;
    mutable std::string bytes_;
    mutable bool        resolved_ = true;
};

} // namespace agentty
