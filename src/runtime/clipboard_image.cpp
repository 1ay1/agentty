#include "agentty/runtime/clipboard_image.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#if defined(_WIN32)
  #define AGENTTY_POPEN  ::_popen
  #define AGENTTY_PCLOSE ::_pclose
  #define AGENTTY_POPEN_MODE "rb"

  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  #ifndef NOMINMAX
    #define NOMINMAX
  #endif
  #include <windows.h>
  #include <objidl.h>     // IStream
  #include <shlwapi.h>    // SHCreateMemStream
  #include <gdiplus.h>
#else
  #define AGENTTY_POPEN  ::popen
  #define AGENTTY_PCLOSE ::pclose
  #define AGENTTY_POPEN_MODE "r"
#endif

namespace agentty {

namespace {

// Anthropic's per-image cap is 5 MB; raw 8 MiB after base64 expansion
// is the most we'd ship. Bigger captures get truncated to 8 MiB at
// the read boundary — magic-byte sniff still works on the prefix.
constexpr std::size_t kCap = 8 * 1024 * 1024;

// Run a shell command and capture binary stdout up to `cap` bytes.
// Returns the bytes plus the wait-status. status==-1 means popen
// failed outright (rare — fork/exec issues).
struct CaptureResult {
    std::string bytes;
    int         status = -1;
};

CaptureResult popen_capture(const char* cmd, std::size_t cap) {
    CaptureResult r;
    FILE* fp = AGENTTY_POPEN(cmd, AGENTTY_POPEN_MODE);
    if (!fp) return r;
    r.bytes.reserve(std::min<std::size_t>(cap, 256 * 1024));
    char buf[8192];
    while (r.bytes.size() < cap) {
        std::size_t avail = cap - r.bytes.size();
        std::size_t want  = avail < sizeof(buf) ? avail : sizeof(buf);
        std::size_t n = std::fread(buf, 1, want, fp);
        if (n == 0) break;
        r.bytes.append(buf, n);
    }
    r.status = AGENTTY_PCLOSE(fp);
    return r;
}

const char* sniff_image_type(std::string_view bytes) {
    auto u = [&](std::size_t i){ return static_cast<unsigned char>(bytes[i]); };
    if (bytes.size() >= 8 && u(0) == 0x89 && u(1) == 0x50 && u(2) == 0x4E
        && u(3) == 0x47 && u(4) == 0x0D && u(5) == 0x0A && u(6) == 0x1A
        && u(7) == 0x0A) return "image/png";
    if (bytes.size() >= 3 && u(0) == 0xFF && u(1) == 0xD8 && u(2) == 0xFF)
        return "image/jpeg";
    if (bytes.size() >= 6
        && bytes[0] == 'G' && bytes[1] == 'I' && bytes[2] == 'F'
        && bytes[3] == '8' && (bytes[4] == '7' || bytes[4] == '9')
        && bytes[5] == 'a') return "image/gif";
    if (bytes.size() >= 12
        && bytes[0] == 'R' && bytes[1] == 'I' && bytes[2] == 'F'
        && bytes[3] == 'F' && bytes[8] == 'W' && bytes[9] == 'E'
        && bytes[10] == 'B' && bytes[11] == 'P') return "image/webp";
    return nullptr;
}

bool tool_in_path(const char* name) {
#if defined(_WIN32)
    // `where` is the Windows analogue of `command -v`.
    std::string cmd = "where ";
    cmd += name;
    cmd += " >NUL 2>&1";
#else
    std::string cmd = "command -v ";
    cmd += name;
    cmd += " >/dev/null 2>&1";
#endif
    return std::system(cmd.c_str()) == 0;
}

// Pick the best image-class MIME type from a newline-separated
// listing the clipboard advertises. Preference order matches the
// quality that survives a round trip through Anthropic's image
// resizing: lossless first (PNG / WEBP-lossless), then lossy. Note
// that some Qt apps publish only `application/x-qt-image`, which
// looks image-y but doesn't actually carry decodable PNG/JPEG bytes
// — we explicitly skip it.
const char* pick_clipboard_image_type(std::string_view types) {
    static const char* prefs[] = {
        "image/png",
        "image/jpeg", "image/jpg",
        "image/webp",
        "image/gif",
        "image/bmp",
        "image/tiff",
    };
    for (const char* p : prefs) {
        // Substring match is fine — the listing is one MIME per
        // line, and "image/jpeg" never appears as a substring of a
        // longer image MIME we care about.
        if (types.find(p) != std::string_view::npos) return p;
    }
    return nullptr;
}

bool clipboard_has_qt_image_only(std::string_view types) {
    if (types.find("application/x-qt-image") == std::string_view::npos)
        return false;
    // Treat Qt-image as the only image-class advertisement when no
    // standard image/* type is also present. Some Qt apps publish
    // both (image/png + application/x-qt-image), in which case the
    // png path picks it up and we never reach this branch.
    return pick_clipboard_image_type(types) == nullptr;
}

std::optional<ClipboardImage> wrap(CaptureResult r) {
    if (r.status != 0 || r.bytes.empty()) return std::nullopt;
    auto* mt = sniff_image_type(r.bytes);
    if (!mt) return std::nullopt;
    ClipboardImage img;
    img.bytes      = std::move(r.bytes);
    img.media_type = mt;
    return img;
}

} // namespace

std::optional<ClipboardImage> read_clipboard_image(std::string* error_out) {
    auto fail = [&](const char* msg) -> std::optional<ClipboardImage> {
        if (error_out) *error_out = msg;
        return std::nullopt;
    };
    auto fail_owned = [&](std::string msg) -> std::optional<ClipboardImage> {
        if (error_out) *error_out = std::move(msg);
        return std::nullopt;
    };

#if defined(__linux__)
    // Session-type detection. KDE / GNOME / sway / etc. on Wayland
    // all set XDG_SESSION_TYPE; the WAYLAND_DISPLAY fallback catches
    // edge cases where the env var got unset by a shell rc.
    bool wayland = false;
    if (const char* st = std::getenv("XDG_SESSION_TYPE"))
        wayland = std::string_view{st} == "wayland";
    if (const char* w = std::getenv("WAYLAND_DISPLAY"); w && *w) wayland = true;

    bool has_wl_paste = tool_in_path("wl-paste");
    bool has_xclip    = tool_in_path("xclip");

    if (!has_wl_paste && !has_xclip) {
        return fail(wayland
            ? "no clipboard tool — install wl-clipboard "
              "(`pacman -S wl-clipboard` / `apt install wl-clipboard`)"
            : "no clipboard tool — install xclip "
              "(`pacman -S xclip` / `apt install xclip`)");
    }

    // ---- Wayland path: prefer wl-paste --------------------------------
    if (wayland && has_wl_paste) {
        // Discover what the clipboard is actually offering. wl-paste
        // exits non-zero on an empty clipboard; capture even on
        // failure so we can give a precise diagnostic.
        auto types_r = popen_capture("wl-paste --list-types 2>/dev/null", 64 * 1024);
        if (types_r.bytes.empty()) {
            // wl-paste returned nothing — empty clipboard, OR
            // wl-paste needs a connection it can't get.
            // Try xclip fallback if installed.
            if (!has_xclip)
                return fail("clipboard is empty");
            // fall through to xclip path below
        } else if (auto* mime = pick_clipboard_image_type(types_r.bytes)) {
            std::string cmd = "wl-paste --type ";
            cmd += mime;
            cmd += " 2>/dev/null";
            if (auto img = wrap(popen_capture(cmd.c_str(), kCap)))
                return img;
            // Listed but failed to capture — usually means the
            // source app died between list and read (KDE's Klipper
            // can race here on Wayland).
            return fail_owned(std::string{
                "clipboard advertised "} + mime
                + " but the bytes were unavailable (source app may have closed)");
        } else if (clipboard_has_qt_image_only(types_r.bytes)) {
            return fail("clipboard image is in Qt-internal format only "
                        "(application/x-qt-image) — copy from a non-Qt app, "
                        "or take the screenshot via Spectacle's \"Save to "
                        "clipboard\" with the PNG default");
        }
        // No image-class MIME on the wayland clipboard. Fall through
        // to xclip in case Klipper's X11 bridge has it.
    }

    // ---- X11 path (also runs as fallback on Wayland) ------------------
    if (has_xclip) {
        auto targets = popen_capture(
            "xclip -selection clipboard -t TARGETS -o 2>/dev/null",
            64 * 1024);
        if (auto* mime = pick_clipboard_image_type(targets.bytes)) {
            std::string cmd = "xclip -selection clipboard -t ";
            cmd += mime;
            cmd += " -o 2>/dev/null";
            if (auto img = wrap(popen_capture(cmd.c_str(), kCap)))
                return img;
        }
        if (clipboard_has_qt_image_only(targets.bytes)) {
            return fail("clipboard image is in Qt-internal format only "
                        "(application/x-qt-image) — install wl-clipboard for "
                        "Wayland-native access");
        }
    }

    // Both tools tried; nothing image-y came back.
    if (wayland && !has_wl_paste) {
        return fail("clipboard has no image — Wayland session needs "
                    "wl-clipboard for native access (xclip works only "
                    "via Klipper's X11 bridge, which doesn't always "
                    "carry images)");
    }
    return fail("clipboard has no image");

#elif defined(__APPLE__)
    if (tool_in_path("pngpaste")) {
        if (auto img = wrap(popen_capture("pngpaste - 2>/dev/null", kCap)))
            return img;
        return fail("clipboard has no image");
    }
    // osascript fallback — slower (~150-300 ms) but always available.
    auto r = popen_capture(
        "set -e; "
        "f=$(mktemp -t agentty-clip).png; "
        "trap 'rm -f \"$f\"' EXIT; "
        "osascript -e 'set png to (the clipboard as «class PNGf»)' "
        "          -e 'set fh to open for access POSIX file \"'\"$f\"'\" "
        "                 with write permission' "
        "          -e 'write png to fh' "
        "          -e 'close access fh' >/dev/null 2>&1 && cat \"$f\"",
        kCap);
    if (auto img = wrap(std::move(r))) return img;
    return fail("clipboard has no image (install pngpaste for a faster path: "
                "`brew install pngpaste`)");

#elif defined(_WIN32)
    // Direct Win32 Clipboard API. The previous PowerShell shell-out
    // had two failure modes that made Ctrl+V silently no-op:
    //
    //   1. Quoting: _popen launches `cmd /C <our string>`. The full
    //      script contained `(`, `)`, `;`, `$`, `[`, `]` — characters
    //      cmd treats as either special or shell-meta depending on the
    //      parent process's environment. Under MSYS2 / Git Bash, the
    //      command was visibly mangled before reaching powershell.
    //
    //   2. PowerShell startup tax: even when quoting worked, the user
    //      paid 200–500 ms of PS spin-up per paste. On a healthy
    //      clipboard the diagnostic was `wrap()` returning nullopt
    //      because of a non-zero status, which surfaced as the generic
    //      "no image on clipboard" toast — hiding the real cause.
    //
    // Going through user32/gdiplus eliminates the shell entirely. ~ms-
    // scale paste, precise errors, no external dependency.

    if (!::OpenClipboard(nullptr))
        return fail("could not open Windows clipboard (another process may hold it)");
    struct ClipGuard {
        ~ClipGuard() { ::CloseClipboard(); }
    } clip_guard;

    // (a) Fast path: clipboard already carries native PNG bytes. Browsers,
    //     Discord, Slack, ShareX, Greenshot, etc. register the "PNG"
    //     format alongside CF_DIB so we can copy without re-encoding.
    if (UINT png_fmt = ::RegisterClipboardFormatW(L"PNG");
        png_fmt && ::IsClipboardFormatAvailable(png_fmt))
    {
        if (HANDLE h = ::GetClipboardData(png_fmt); h) {
            auto* data = static_cast<const char*>(::GlobalLock(h));
            const SIZE_T size = data ? ::GlobalSize(h) : 0;
            if (data && size > 0) {
                std::string bytes(data,
                                  std::min<std::size_t>(size, kCap));
                ::GlobalUnlock(h);
                if (auto img = wrap(CaptureResult{std::move(bytes), 0}))
                    return img;
            } else if (data) {
                ::GlobalUnlock(h);
            }
        }
    }

    // (b) DIB fallback: the standard "Win+Shift+S / Snipping Tool /
    //     PrintScreen" path. Re-attach a BITMAPFILEHEADER so the bytes
    //     are a complete BMP file, then re-encode through GDI+ to PNG
    //     (Anthropic's image API doesn't accept BMP).

    const UINT dib_fmt =
          ::IsClipboardFormatAvailable(CF_DIBV5) ? CF_DIBV5
        : ::IsClipboardFormatAvailable(CF_DIB)   ? CF_DIB
        : 0u;
    if (dib_fmt == 0)
        return fail("clipboard has no image (no PNG/DIB/DIBV5 format present)");

    HANDLE h = ::GetClipboardData(dib_fmt);
    if (!h)
        return fail("GetClipboardData returned NULL");

    auto* dib = static_cast<const BYTE*>(::GlobalLock(h));
    const SIZE_T dib_size = dib ? ::GlobalSize(h) : 0;
    struct UnlockGuard {
        HANDLE h;
        ~UnlockGuard() { if (h) ::GlobalUnlock(h); }
    } unlock_guard{h};
    if (!dib || dib_size < sizeof(BITMAPINFOHEADER))
        return fail("clipboard DIB header is malformed");

    // Compute where the pixel array starts inside the DIB block. For a
    // file-on-disk BMP this is BITMAPFILEHEADER.bfOffBits; here we have
    // to derive it from the info-header + optional palette / bitfield
    // masks. Getting this wrong shifts the pixels and GDI+ either rejects
    // the stream or produces a mangled image.
    const auto* hdr = reinterpret_cast<const BITMAPINFOHEADER*>(dib);
    DWORD palette_bytes = 0;
    if (hdr->biBitCount <= 8) {
        const DWORD n = hdr->biClrUsed ? hdr->biClrUsed
                                       : (1u << hdr->biBitCount);
        palette_bytes = n * sizeof(RGBQUAD);
    } else if (hdr->biCompression == BI_BITFIELDS) {
        palette_bytes = 3 * sizeof(DWORD);
    }
    const DWORD pixels_offset =
        static_cast<DWORD>(sizeof(BITMAPFILEHEADER))
      + hdr->biSize + palette_bytes;

    std::string bmp;
    bmp.resize(sizeof(BITMAPFILEHEADER) + dib_size);
    BITMAPFILEHEADER bfh{};
    bfh.bfType    = 0x4D42; // 'BM'
    bfh.bfSize    = static_cast<DWORD>(bmp.size());
    bfh.bfOffBits = pixels_offset;
    std::memcpy(bmp.data(), &bfh, sizeof(bfh));
    std::memcpy(bmp.data() + sizeof(bfh), dib, dib_size);

    // GDI+ initialise per-call. A process-wide startup at main() would
    // shave ~1 ms but adds a lifecycle the rest of the app doesn't need.
    Gdiplus::GdiplusStartupInput gdi_in;
    ULONG_PTR gdi_token = 0;
    if (Gdiplus::GdiplusStartup(&gdi_token, &gdi_in, nullptr) != Gdiplus::Ok)
        return fail("GDI+ startup failed");
    struct GdiGuard {
        ULONG_PTR tok;
        ~GdiGuard() { Gdiplus::GdiplusShutdown(tok); }
    } gdi_guard{gdi_token};

    IStream* bmp_stream = ::SHCreateMemStream(
        reinterpret_cast<const BYTE*>(bmp.data()),
        static_cast<UINT>(bmp.size()));
    if (!bmp_stream)
        return fail("SHCreateMemStream(bmp) failed");
    struct StreamGuard {
        IStream* s;
        ~StreamGuard() { if (s) s->Release(); }
    } bmp_guard{bmp_stream};

    std::unique_ptr<Gdiplus::Bitmap> bitmap{
        Gdiplus::Bitmap::FromStream(bmp_stream)};
    if (!bitmap || bitmap->GetLastStatus() != Gdiplus::Ok)
        return fail("GDI+ could not decode clipboard DIB");

    // Locate the PNG encoder. Iterating the codec list is the standard
    // GDI+ idiom (there is no GetEncoderByMime helper).
    UINT num_codecs = 0, codec_buf_size = 0;
    Gdiplus::GetImageEncodersSize(&num_codecs, &codec_buf_size);
    if (num_codecs == 0 || codec_buf_size == 0)
        return fail("GDI+ image encoders not available");
    std::vector<BYTE> codec_buf(codec_buf_size);
    auto* codecs = reinterpret_cast<Gdiplus::ImageCodecInfo*>(codec_buf.data());
    Gdiplus::GetImageEncoders(num_codecs, codec_buf_size, codecs);
    CLSID png_clsid{};
    bool found_png = false;
    for (UINT i = 0; i < num_codecs; ++i) {
        if (std::wcscmp(codecs[i].MimeType, L"image/png") == 0) {
            png_clsid  = codecs[i].Clsid;
            found_png  = true;
            break;
        }
    }
    if (!found_png)
        return fail("GDI+ PNG encoder not registered on this system");

    IStream* out_stream = ::SHCreateMemStream(nullptr, 0);
    if (!out_stream)
        return fail("SHCreateMemStream(out) failed");
    struct StreamGuard2 {
        IStream* s;
        ~StreamGuard2() { if (s) s->Release(); }
    } out_guard{out_stream};

    if (bitmap->Save(out_stream, &png_clsid, nullptr) != Gdiplus::Ok)
        return fail("GDI+ PNG encode failed");

    // Drain the IStream into a std::string. Stat() gives the size,
    // Seek() rewinds, Read() copies — standard COM stream dance.
    STATSTG stat{};
    if (out_stream->Stat(&stat, STATFLAG_NONAME) != S_OK)
        return fail("PNG stream Stat failed");
    const auto png_size = static_cast<std::size_t>(stat.cbSize.QuadPart);
    if (png_size == 0 || png_size > kCap)
        return fail("PNG output size out of range");
    LARGE_INTEGER zero{};
    out_stream->Seek(zero, STREAM_SEEK_SET, nullptr);
    std::string png_bytes(png_size, '\0');
    ULONG bytes_read = 0;
    if (out_stream->Read(png_bytes.data(),
                         static_cast<ULONG>(png_size),
                         &bytes_read) != S_OK)
        return fail("PNG stream Read failed");
    png_bytes.resize(bytes_read);

    if (auto img = wrap(CaptureResult{std::move(png_bytes), 0}))
        return img;
    return fail("clipboard image decoded but produced invalid PNG");
#else
    return fail("clipboard image read not implemented on this platform");
#endif
}

} // namespace agentty
