# Build the Ocean compiler with build metadata injected via -ldflags -X.
# OCEAN is the bootstrap compiler (an installed ocean, or a previous build).
OCEAN ?= ocean
VERSION ?= dev
BUNDLE_VERSION ?= $(if $(filter dev,$(VERSION)),$(shell git describe --tags --match 'nightly-*' --abbrev=0 2>/dev/null),$(VERSION))
# Windows builds use GNU Make in an MSYS2 CLANG64/CLANGARM64 shell.
EXE := $(if $(filter Windows_NT,$(OS)),.exe,)
OUT ?= build/ocean$(EXE)
STAGE2 ?= build/ocean-stage2$(EXE)
EMBED := build/embed-runtime$(EXE)
LOGO := build/embed-logo$(EXE)
PKG := github.com/ocean-foundation/ocean/compiler/driver
COMMIT ?= $(shell git rev-parse HEAD 2>/dev/null || echo unknown)
COMMIT_DATE ?= $(shell git log -1 --format=%cs 2>/dev/null || echo unknown)
COMMIT_YEAR ?= $(shell git log -1 --format=%cd --date=format:%Y 2>/dev/null || date -u +%Y)
BUILD_DATE ?= $(shell date -u +%Y-%m-%dT%H:%M:%SZ)
LDFLAGS := -X $(PKG).version=$(VERSION) -X $(PKG).commit=$(COMMIT) \
	-X $(PKG).commitDate=$(COMMIT_DATE) -X $(PKG).commitYear=$(COMMIT_YEAR) \
	-X $(PKG).buildDate=$(BUILD_DATE) -X $(PKG).bundleVersion=$(BUNDLE_VERSION)

.PHONY: all test selfhost clean runtime logo crosscheck release
all: runtime logo
	mkdir -p "$(dir $(OUT))"
	"$(OCEAN)" build -ldflags "$(LDFLAGS)" -o "$(OUT)" .

# Regenerate the embedded runtime after editing compiler/runtime/*.c.
runtime:
	mkdir -p build
	"$(OCEAN)" build -o "$(EMBED)" tools/embedruntime
	"$(EMBED)"

# Embed logo.ansi in logo.oc for CLI printing of Ocean logo ASCII art
logo:
	mkdir -p build
	"$(OCEAN)" build -o "$(LOGO)" tools/embedlogo
	"$(LOGO)"

test: all
	"$(OUT)" test ./... --notty

# Rebuild the compiler with itself.
selfhost: all
	mkdir -p "$(dir $(STAGE2))"
	"$(OUT)" build -ldflags "$(LDFLAGS)" -o "$(STAGE2)" .

# Compile the compiler (and any CROSSCHECK_DIRS) for every native target
# without that machine: OCOS/OCARCH select the target's IR and asm,
# then clang assembles it. Linking still needs the target's SDK/sysroot.
CROSS_TARGETS ?= x86_64-linux aarch64-linux riscv64-linux x86_64-macos aarch64-macos x86_64-windows aarch64-windows
CROSSCHECK_DIRS ?= .
crosscheck: all
	mkdir -p build/cross
	@set -e; for dir in $(CROSSCHECK_DIRS); do for t in $(CROSS_TARGETS); do \
		name=build/cross/$$(echo $$dir | tr '/.' '__')-$$t; \
		OCOS=$${t#*-} OCARCH=$${t%-*} OCEAN_BUILD_CACHE=off "$(OUT)" build --emit-llvm -o $$name.ll $$dir; \
		triple=$$(sed -n 's/^target triple = "\(.*\)"/\1/p' $$name.ll); \
		"$${OCEAN_CLANG:-clang}" --target=$$triple -c -w -O1 -x ir $$name.ll -o $$name.o; \
		echo "ok   $$dir $$t"; \
	done; done
	@echo "OCOS=js OCARCH=wasm"; OCOS=js OCARCH=wasm OCEAN_BUILD_CACHE=off "$(OUT)" build --emit-llvm -o build/cross/wasm.ll . && "$${OCEAN_CLANG:-clang}" --target=wasm32-unknown-unknown -c -w -O1 -x ir build/cross/wasm.ll -o build/cross/wasm.o && echo "ok   . wasm"

clean:
	rm -rf build

release:
	$(MAKE) all OCOS= OCARCH= OCEAN_CLANG= OCEAN_SYSROOT=
	mkdir -p build/release
	@set -e; for t in $(CROSS_TARGETS); do \
		arch=$${t%-*}; \
		os=$${t#*-}; \
		ext=$$( [ "$$os" = "windows" ] && echo .exe || true ); \
		out="build/release/ocean-$(VERSION)-$$os-$$arch$$ext"; \
		OCOS=$$os OCARCH=$$arch "$(OUT)" build $(RELEASE_FLAGS) -ldflags "$(LDFLAGS)" -o "$$out" .; \
		echo "built $$out"; \
	done
