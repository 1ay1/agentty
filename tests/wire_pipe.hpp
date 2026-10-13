// tests/wire_pipe.hpp -- a real maya::Writer over a real OS pipe, for wire
// tests that render frames and inspect the exact bytes that hit the "tty".
//
//     auto [writer, rfd] = wire_pipe::make_writer();
//     ... render a frame through writer ...
//     std::string bytes = wire_pipe::drain(rfd);   // never blocks
//     ::close(rfd);
//
// POSIX: pipe(2), both ends O_NONBLOCK; drain reads until EAGAIN.
// Windows: an anonymous CRT pipe with a large buffer (WriteFile on an
// anonymous pipe blocks when full, so the buffer must hold a whole frame —
// tests drain after every frame); drain asks PeekNamedPipe how much is
// queued and reads exactly that, so it never blocks on an empty pipe.
//
// The write end stays open for the process lifetime on both platforms (the
// Writer doesn't own its handle), matching the behaviour these tests had.
#pragma once

#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <utility>

#include <maya/platform/io.hpp>
#include <maya/terminal/writer.hpp>

#if defined(_WIN32)
#  include <fcntl.h>
#  include <io.h>
#  include <windows.h>
#else
#  include <fcntl.h>
#  include <unistd.h>
#endif

namespace wire_pipe {

#if defined(_WIN32)
// Larger than any single frame the wire tests emit (full-screen redraws
// of wide fuzz terminals are a few hundred KB).
inline constexpr unsigned kPipeBytes = 16u << 20;
#endif

[[nodiscard]] inline std::pair<maya::Writer, int> make_writer() {
    int fds[2];
#if defined(_WIN32)
    if (::_pipe(fds, kPipeBytes, _O_BINARY | _O_NOINHERIT) != 0) { std::perror("_pipe"); std::abort(); }
    const auto h = reinterpret_cast<maya::platform::NativeHandle>(::_get_osfhandle(fds[1]));
    return {maya::Writer{h}, fds[0]};
#else
    if (::pipe(fds) != 0) { std::perror("pipe"); std::abort(); }
    ::fcntl(fds[1], F_SETFL, ::fcntl(fds[1], F_GETFL, 0) | O_NONBLOCK);
    ::fcntl(fds[0], F_SETFL, ::fcntl(fds[0], F_GETFL, 0) | O_NONBLOCK);
    return {maya::Writer{static_cast<maya::platform::NativeHandle>(fds[1])}, fds[0]};
#endif
}

// Everything currently queued on the read end; returns "" when empty.
[[nodiscard]] inline std::string drain(int rfd) {
    std::string out;
    char buf[8192];
#if defined(_WIN32)
    const auto h = reinterpret_cast<HANDLE>(::_get_osfhandle(rfd));
    for (;;) {
        DWORD avail = 0;
        if (!::PeekNamedPipe(h, nullptr, 0, nullptr, &avail, nullptr) || avail == 0)
            break;
        const unsigned want = avail < sizeof(buf) ? static_cast<unsigned>(avail)
                                                  : static_cast<unsigned>(sizeof(buf));
        const int n = ::_read(rfd, buf, want);
        if (n <= 0) break;
        out.append(buf, static_cast<std::size_t>(n));
    }
#else
    ssize_t n;
    while ((n = ::read(rfd, buf, sizeof(buf))) > 0)
        out.append(buf, static_cast<std::size_t>(n));
#endif
    return out;
}

}  // namespace wire_pipe
