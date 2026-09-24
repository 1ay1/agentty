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
//   - Thread-safe. bytes() is const and is called from worker threads
//     (the provider transports build the wire body off the UI thread, and
//     the persistence writer saves on its own thread). It used to write two
//     `mutable` fields with no synchronisation, so two threads reading the
//     same unresolved payload could both write bytes_: a data race that
//     can tear a std::string. Now the payload lives in a shared, immutable
//     cell and materialises exactly once under std::call_once. Copies
//     share the cell, so they resolve once between them (and copying a
//     Thread for a turn no longer deep-copies image bytes). set_bytes()
//     never touches a shared cell: it gives this handle a new one, so other
//     copies keep their value.
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

#include <atomic>
#include <memory>
#include <mutex>
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
        : cell_(std::make_shared<const Cell>(Cell::resolved_tag{}, std::move(raw))) {}

    [[nodiscard]] static LazyBytes lazy(Source src) {
        LazyBytes v;
        if (!src.empty())                  // nothing to resolve => stays empty
            v.cell_ = std::make_shared<const Cell>(std::move(src));
        return v;
    }
    [[nodiscard]] static LazyBytes from_blob(std::string name) {
        return lazy(Source{.blob = std::move(name), .b64 = {}});
    }
    [[nodiscard]] static LazyBytes from_base64(std::string b64) {
        return lazy(Source{.blob = {}, .b64 = std::move(b64)});
    }

    // The raw bytes, materialising them on first use. Safe to call from
    // any thread, on any copy, concurrently.
    [[nodiscard]] const std::string& bytes() const {
        return cell_ ? cell_->bytes() : empty_string();
    }

    // Replace the payload outright (clipboard capture, tool result, an
    // @file read at submit time). Gives this handle its own cell; copies
    // made earlier keep the old payload.
    void set_bytes(std::string raw) {
        cell_ = std::make_shared<const Cell>(Cell::resolved_tag{}, std::move(raw));
    }

    // The pending source, for the writer: a payload that was never
    // materialised can be re-persisted BY REFERENCE, with no decode and no
    // re-encode. Empty for payloads built from raw bytes or set_bytes().
    // Callers check materialised() first; the source itself stays readable
    // after materialisation, as before.
    [[nodiscard]] const Source& source() const noexcept {
        return cell_ ? cell_->source : empty_source();
    }
    [[nodiscard]] bool materialised() const noexcept {
        return !cell_ || cell_->done.load(std::memory_order_acquire);
    }

    // "Is there nothing here?" without forcing a blob read: an
    // unmaterialised payload with a source is not empty, whatever the
    // source resolves to.
    [[nodiscard]] bool empty() const noexcept {
        if (!cell_) return true;
        return materialised() ? cell_->bytes().empty() : cell_->source.empty();
    }

    // Installed once at startup by the persistence layer. A null resolver
    // (a unit test that never loads from disk) makes every lazy payload
    // resolve to empty, which is the same as a missing blob. Atomic because
    // workers read it; it's written once, before any worker exists.
    using Resolver = std::string (*)(const Source&);
    static void set_resolver(Resolver r) noexcept {
        resolver_.store(r, std::memory_order_release);
    }

private:
    // Immutable once shared, except the one-time materialisation, which
    // std::call_once makes race-free: every thread either runs the resolve
    // or waits for it, and all see the finished bytes afterwards.
    struct Cell {
        struct resolved_tag {};
        explicit Cell(Source s) : source(std::move(s)) {}
        Cell(resolved_tag, std::string raw) : bytes_(std::move(raw)) {
            done.store(true, std::memory_order_relaxed);
        }

        const std::string& bytes() const {
            if (!done.load(std::memory_order_acquire)) {
                std::call_once(once_, [this] {
                    auto r  = resolver_.load(std::memory_order_acquire);
                    bytes_  = r ? r(source) : std::string{};
                    done.store(true, std::memory_order_release);
                });
            }
            return bytes_;
        }

        const Source              source;
        mutable std::atomic<bool> done{false};
    private:
        mutable std::once_flag    once_;
        mutable std::string       bytes_;     // written once, inside call_once
    };

    static const std::string& empty_string() noexcept {
        static const std::string s;
        return s;
    }
    static const Source& empty_source() noexcept {
        static const Source s{};
        return s;
    }

    inline static std::atomic<Resolver> resolver_{nullptr};

    std::shared_ptr<const Cell> cell_;    // null = empty, resolved
};

} // namespace agentty
