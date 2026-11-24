# Building Ocean

The build entry point is GNU Make. Build helpers and compiler portability code
are written in Ocean; Python is not required.

You need an installed bootstrap `ocean` compiler with its matching standard
library, Clang, libffi development files, `pkg-config`, Git, and Make. The existing
embedded runtime is compiled by Clang as part of each native program.

## macOS

Use the Xcode command-line tools or a native LLVM installation. Make sure
`pkg-config --cflags --libs libffi` succeeds so the compiler can locate libffi in
the SDK or a separate installation.

```sh
make
make selfhost
make test
```

The compiler is `build/ocean`; the self-hosted compiler is `build/ocean-stage2`.

## Windows

Run GNU Make from an MSYS2 **CLANG64** (x86-64) or **CLANGARM64** (ARM64) shell.
Use the matching native MinGW Clang toolchain, libffi, and `pkg-config`, with
`clang`, `llvm-ar`, and the bootstrap `ocean.exe` on PATH. The Makefile uses the
MSYS2 shell utilities for directory creation, Git metadata, and UTC dates. It is
not a build for MSVC, PowerShell, or cmd.exe's shell syntax.

```sh
make
make selfhost
make test
```

The compiler is `build/ocean.exe`; the self-hosted compiler is
`build/ocean-stage2.exe`. Windows library artifacts use indexed COFF archives.
The Windows test runner uses the final report rather than the live terminal
display. LSP analysis runs in the server process because Windows has no `fork`.

## Bootstrap and standard-library location

Override the bootstrap executable if needed:

```sh
make OCEAN=/path/to/ocean VERSION=dev
```

The compiler is built with the standard library's `strings`, `strconv`, `sort`,
`unicode`, `path`, `os`, `os/signal` and `time` packages, so the bootstrap's
library must provide them: a matching release or nightly, or `OCEAN_SRC` set to
a current `ocean-src` checkout.

For a checkout whose standard library is not installed beside the executable,
set `OCEAN_SRC` to the matching standard-library source directory before building
or testing. On Windows, use a native path such as `C:/ocean/src`, including in an
MSYS2 shell. `OUT` and `STAGE2` override the output paths.

`make runtime` rebuilds and runs `tools/embedruntime` to refresh
`compiler/runtime/source.oc`. It preserves the file when its contents have not
changed. `make clean` removes the build directory.
