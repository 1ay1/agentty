// tests/win_posix_compat.hpp -- POSIX environment calls for the test suite on
// Windows (MinGW/UCRT).
//
// The suite points the process at temp HOME / workspace dirs with setenv and
// unsetenv, which UCRT doesn't declare. Force-included into every test TU on
// Windows (CMakeLists.txt, AGENTTY_BUILD_TESTS block), so the tests stay
// written against POSIX and the shim lives in one place. Not used by the
// product: src/ never sets environment variables after startup.
//
// Tests that need more of POSIX are made portable at the call site instead
// (tests/wire_pipe.hpp for pipe+Writer, #ifdef SIGPIPE, a watchdog thread
// for alarm). The openpty-driven tests are UNIX-only in AgenttyTests.cmake.
#pragma once

#if defined(_WIN32)
#include <cstdlib>
#include <string>

inline int setenv(const char* name, const char* value, int overwrite) {
    if (!name || !*name) return -1;
    if (!overwrite && std::getenv(name)) return 0;
    return _putenv_s(name, value ? value : "");
}

inline int unsetenv(const char* name) {
    if (!name || !*name) return -1;
    // An empty value removes the variable under _putenv_s.
    return _putenv_s(name, "");
}

// Tests write `setenv("HOME", dir.c_str(), 1)` with a std::filesystem::path,
// whose c_str() is wchar_t* on Windows. Same meaning, wide value.
inline int setenv(const char* name, const wchar_t* value, int overwrite) {
    if (!name || !*name) return -1;
    if (!overwrite && std::getenv(name)) return 0;
    const std::wstring wname(name, name + std::char_traits<char>::length(name));
    return _wputenv_s(wname.c_str(), value ? value : L"");
}
#endif
