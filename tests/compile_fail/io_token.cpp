// Compile-fail cases for agentty::Io. Case 0 must compile (it proves the
// harness and includes are fine); every other case must NOT.
#include "agentty/util/io.hpp"

namespace demo {
// Stands in for any IO function: it takes an Io.
inline int read_disk(agentty::Io) { return 1; }
struct Model { int n = 0; };
}  // namespace demo

#if IO_CASE == 0
// An effect body that was handed an Io may call it.
int effect(agentty::Io io) { return demo::read_disk(io); }
int main() { return 0; }
#elif IO_CASE == 1
// A reducer has no Io, so it can't call an IO function.
void update(demo::Model& m) { m.n = demo::read_disk(); }
#elif IO_CASE == 2
// Nor make one: the constructor is private.
void update(demo::Model& m) { m.n = demo::read_disk(agentty::Io{}); }
#else
#error "no case selected"
#endif
