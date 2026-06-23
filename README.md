# BASIC09 Compiler Prototype

`basic09c` is an experimental standalone BASIC09 frontend that uses LLVM as a
library. It reads BASIC09 source, builds an AST, performs initial semantic
checks, and can lower the supported language subset to LLVM IR.

The compiler is target-independent. It does not depend on the MC6809 backend,
OS-9 runtime support, or an emulator.

## Build

Point CMake at an LLVM build or install tree that provides `LLVMConfig.cmake`:

```sh
cmake -S /Volumes/Lagniappe/basic09c -B /Volumes/Lagniappe/basic09c/build -G Ninja \
  -DLLVM_DIR=/Volumes/Lagniappe/llvm-project/build/lib/cmake/llvm \
  -DCMAKE_BUILD_TYPE=Release

ninja -C /Volumes/Lagniappe/basic09c/build basic09c
```

The compiler is written to:

```sh
/Volumes/Lagniappe/basic09c/build/bin/basic09c
```

## Usage

```sh
/Volumes/Lagniappe/basic09c/build/bin/basic09c --dump-tokens path/to/program.b09
/Volumes/Lagniappe/basic09c/build/bin/basic09c --dump-ast path/to/program.b09
/Volumes/Lagniappe/basic09c/build/bin/basic09c --syntax-only path/to/program.b09
/Volumes/Lagniappe/basic09c/build/bin/basic09c --analyze-only path/to/program.b09
/Volumes/Lagniappe/basic09c/build/bin/basic09c --dump-symbols path/to/program.b09
/Volumes/Lagniappe/basic09c/build/bin/basic09c --emit-llvm path/to/program.b09
```

## Tests

Run the standalone test suite with:

```sh
ninja -C /Volumes/Lagniappe/basic09c/build check-basic09c
```

Most tests only need `basic09c` plus LLVM test utilities such as `FileCheck`,
`split-file`, and `not`.

The optional MAME/CoCo 3 tests run only when MAME, Toolshed `os9`, and a usable
CoCo 3 ROM path are available:

```sh
MAME_ROM_PATH=/path/to/mame/roms \
  ninja -C /Volumes/Lagniappe/basic09c/build check-basic09c
```

## Source Layout

- `src/Basic09Token.*`: token kinds and token helpers
- `src/Basic09Lexer.*`: source normalization and lexing
- `src/Basic09AST.*`: AST node definitions and AST dumping
- `src/Basic09Parser.*`: recursive-descent parser
- `src/Basic09Semantic.*`: initial semantic checks
- `src/Basic09Symbols.*`: symbol collection and dumping
- `src/Basic09IR.*`: LLVM IR lowering
- `src/basic09c.cpp`: command-line driver
- `test/`: lit tests and BASIC09 sample inputs

## Status

Implemented pieces include source normalization, lexing, parsing, AST dumps,
semantic checks, symbol dumps, and LLVM IR emission for a useful subset of
BASIC09. Runtime behavior is prototype quality and intentionally small.
