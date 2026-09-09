#pragma once
// agentty::blobs — the content-addressed payload store.
//
// Large payloads used to be base64'd INTO the thread JSON. A screenshot is
// ~1 MB, base64 inflates it to ~1.3 MB, and every one of them is then
// re-read and re-decoded on every thread load — forever. One real thread
// reached 29 MB, of which 17 MB was images and only 0.7 MB was actual
// conversation text; switching to it stalled the UI for a fifth of a
// second before a single row was drawn.
//
// Blobs fix the growth at the source: the bytes are written ONCE to
// threads/blobs/<hash>, and the message keeps a reference. Identical
// payloads (the same screenshot pasted twice, or a retried turn) collapse
// to one file for free because the name IS the content hash.
//
// The store is deliberately tiny and knows nothing about what it holds:
// it takes bytes, returns a name, and gives the bytes back. Images use it
// today; tool output and arbitrary file attachments are the same problem
// and will use it unchanged.
//
// Extracted verbatim from persistence.cpp — which had accumulated ten
// separate concerns in one 86 KB file — so the thread log can share it
// without depending on the whole persistence layer. Behaviour is
// identical; the hash function and on-disk layout are unchanged, so every
// blob written by the previous code still resolves.

#include <filesystem>
#include <string>

namespace agentty::blobs {

// threads/blobs/. Created on first use.
[[nodiscard]] std::filesystem::path dir();

// Content hash used as the blob's file name. FNV-1a over the bytes, hex.
// NOT cryptographic — this names a local cache entry, it does not defend
// against an adversary who can already write to the directory.
[[nodiscard]] std::string name_for(std::string_view bytes);

// Write `bytes` and return its name, or an empty string on failure.
// Content-addressed, so an existing blob with the same name is
// byte-identical by construction and is left alone.
//
// Callers MUST handle the empty return by falling back to inlining the
// payload: a slow thread beats a lost image.
[[nodiscard]] std::string put(const std::string& bytes);

// Read a blob by name. Returns empty if the name is malformed, the file
// is missing, or it can't be read — all of which callers already treat as
// "this payload is gone", the same as the pre-blob corrupt-base64 path.
[[nodiscard]] std::string get(const std::string& name);

} // namespace agentty::blobs
