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

## Cross-compilation

Set both `OCOS` and `OCARCH` for `ocean build` and `ocean check`. With neither
variable set, Ocean targets the host. `ocean run` and `ocean test` require the
host target.

| OCOS | OCARCH |
| --- | --- |
| `linux` | `x86_64`, `aarch64`, `riscv64` |
| `macos` | `x86_64`, `aarch64` |
| `windows` | `x86_64`, `aarch64` |
| `js` | `wasm` |

```sh
OCOS=linux OCARCH=aarch64 ocean build -o app .
```

Use `ocean target --list` to check which targets can compile and link with
the toolchains on `PATH` or in the target installation directory. A green
checkmark means the target is ready. A red cross means the toolchain check
failed. Install managed components for targets you need.

```sh
ocean target --list
ocean target x86_64-windows riscv64-linux wasm-js
OCOS=windows OCARCH=x86_64 ocean build -o app.exe .
ocean target --remove x86_64-windows riscv64-linux
```

The installer downloads bundles and SHA-256 checksums from the matching
Ocean GitHub release using `curl`. It verifies the archive, extracts it with
`tar`, and checks that it can link for the target. A development build uses
the closest reachable nightly tag by default. Override the bundle tag with
`make BUNDLE_VERSION=nightly-YYYY.MM.DD`.
Linux and Windows bundles include the target development libraries for libffi
and libcurl. With dynamic linking, the deployed program still needs its
target runtime libraries. macOS targets use the Apple SDK on a Mac. Install
the Command Line Tools with `xcode-select --install` if prompted by the command.

By default the components live in `~/.ocean/targets` on Linux and macOS and
`%LOCALAPPDATA%/Ocean/targets` on Windows. Set `OCEAN_TARGET_DIR` to an absolute
path to use a different location. Removing a target deletes its files and
keeps the shared Clang and linker cache for other targets.

For a manually configured toolchain, use `OCEAN_CLANG` to select a Clang executable or a wrapper configured with
your target's linker flags. Use `OCEAN_SYSROOT` if the target headers and
libraries are in a separate sysroot.

- Linux needs a target-capable linker and the target's libc headers,
  startup objects and compiler runtime libraries. A matching Linux GCC cross
  toolchain can provide these files for Clang.
- macOS needs a macOS SDK and a Mach-O linker. On macOS, install the Xcode
  Command Line Tools with `xcode-select --install`.
- Windows needs a matching MinGW-w64 toolchain such as LLVM-MinGW. Put
  `llvm-ar` on PATH to build package libraries.
- WebAssembly needs Clang with WebAssembly support and the LLD tool `wasm-ld`.

Install target libffi development files for C variadic calls and target libcurl
development files for native HTTP. When using pkg-config for a cross build,
set `PKG_CONFIG_LIBDIR` to the target's metadata directories, keep
`PKG_CONFIG_PATH` free of host entries, and set `PKG_CONFIG_SYSROOT_DIR` if the
metadata paths are relative to a separate sysroot.

With an explicit target, `ocean build --emit-llvm` emits program IR without
requiring the target toolchain. `make crosscheck` also assembles the IR. `make release`
builds the native release matrix in `build/release`. Use
`make release CROSS_TARGETS=aarch64-linux` to build a single release target.

## Releases

The nightly workflow on `main` is scheduled for 00:00 UTC. It releases
when either repository's snapshot differs from the last published release on
`main`, or when 30 days have passed since that release. When a release is due,
the workflow tags the preceding UTC day as `nightly-YYYY.MM.DD` in both Ocean
and `ocean-src`. The workflow uses write-enabled deploy keys stored in Ocean
Actions secrets `NIGHTLY_OCEAN_SSH_KEY` and `NIGHTLY_STDLIB_SSH_KEY`.

Pushing the Ocean tag starts the tagged release workflow. For a stable
release, push a version tag such as `v1.2.3` to `ocean-src` first, then
push the same tag to Ocean. Nightly and version prerelease tags publish as
GitHub prereleases.

[`release-deps.json`](release-deps.json) lists the toolchains and packages
used by CI. Each runner downloads the verified compiler binary from the
previous published release on `main` and runs `make release` for its target.
The first Ocean release bootstraps from the pinned historical `ocean-c`
release. RISC-V builds run under QEMU. WebAssembly builds use the shared host
tools and the embedded runtime.

The workflow publishes the release after all seven native targets
have produced their compiler archives, dependency bundles, and SHA-256
manifests. The compiler archives contain the native executable. Install the
matching `ocean-src` tag alongside it or set `OCEAN_SRC`.
When the host compiler and release target need different pkg-config settings,
build the host compiler with `make`, then invoke `build/ocean build` with the
target settings.
