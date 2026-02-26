# hero_shell

## Build

### Prerequisites

- CMake 3.15+
- C++17 compatible compiler
- make (used by bundled ncurses/libedit builds)

### Steps

```sh
git submodule update --init --recursive

cmake -S . -B build
cmake --build build -j
```

The binary will be generated at `build/hero_shell`.
