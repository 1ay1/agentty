# Chapter 1 example programs

Ten programs, one per section. Each is standalone: no agentty headers, no
build system, nothing to install beyond a C++23 compiler.

## build everything

```sh
make          # builds all ten
make run      # builds and runs all ten in order
make clean
```

## build one

```sh
make 03_strong_types && ./03_strong_types
```

or without make at all:

```sh
g++ -std=c++23 -Wall -Wextra -fsanitize=address,undefined -g \
    03_strong_types.cpp -o 03_strong_types && ./03_strong_types
```

## the files

| file | section |
|------|---------|
| `01_types.cpp` | [1. What a type is](../01-what-is-a-type.md) |
| `02_integers.cpp` | [2. Integers lie](../02-integers-lie.md) |
| `03_strong_types.cpp` | [3. Strong types](../03-strong-types.md) |
| `04_value_categories.cpp` | [4. Value categories](../04-value-categories.md) |
| `05_init.cpp` | [5. Initialisation](../05-initialisation.md) |
| `06_references.cpp` | [6. References and const](../06-references-and-const.md) |
| `07_lifetime.cpp` | [7. Lifetime](../07-lifetime.md) |
| `08_copy_move.cpp` | [8. Copy, move, elision](../08-copy-move-elision.md) |
| `09_auto.cpp` | [9. auto and decltype](../09-auto-and-decltype.md) |
| `10_imagecontent.cpp` | [10. Capstone](../10-capstone-imagecontent.md) |

## about the warnings

Two warnings are deliberate and marked in the source:

- `02_integers.cpp` — the signed/unsigned comparison. The warning is the
  lesson, so it is suppressed with a local pragma and explained inline.
- `04_value_categories.cpp` — a discarded `std::move`, to show it does
  nothing on its own.

Anything else that warns is a bug. Tell me.

## sanitizers

Every program is built with AddressSanitizer and UndefinedBehaviorSanitizer
on. `07_lifetime.cpp` has commented-out dangling examples. Uncomment one,
rebuild, run, and read what ASan says. That output is worth more than any
paragraph about lifetime.
