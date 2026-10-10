#pragma once
// agentty::Io — permission to touch the world, as a value.
//
// A function that does IO or changes process-wide state (reads the
// credential store, walks the skills dir, dials, swaps the active provider)
// takes an `Io` parameter. update() and view() never have one, so they can't
// call it, and neither can any helper they call unless that helper is handed
// an Io too. That is what makes "the reducer reached the disk through a
// helper" a compile error instead of a lint miss.
//
// Only a few places make one, through IoAccess:
//   * effect bodies: cmd::io_task (runtime/cmd.hpp) and the background jobs
//     in util/background.hpp
//   * the host that runs effects, main(), and tool / provider entry points
//   * tests
// tests/lint/io_roots.txt lists them; the io_roots lint fails on any other
// file that names IoAccess, and on a listed file that no longer does.
//
// It is an empty value: free to pass, nothing to store. It is not Sendable
// (jaal can't see inside it), so it can't ride a Msg into the Model, and a
// job makes its own rather than being handed one.

namespace agentty {

class Io {
public:
    Io(const Io&)            = default;
    Io& operator=(const Io&) = default;

private:
    constexpr Io() noexcept = default;
    friend struct IoAccess;
};

struct IoAccess {
    [[nodiscard]] static constexpr Io grant() noexcept { return Io{}; }
};

} // namespace agentty
