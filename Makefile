# Build the Ocean compiler with build metadata injected via -ldflags -X.
# OCEAN is the bootstrap compiler (an installed ocean, or a previous build).
OCEAN ?= ocean
VERSION ?= dev
# Windows builds use GNU Make in an MSYS2 CLANG64/CLANGARM64 shell.
EXE := $(if $(filter Windows_NT,$(OS)),.exe,)
OUT ?= build/ocean$(EXE)
STAGE2 ?= build/ocean-stage2$(EXE)
EMBED := build/embed-runtime$(EXE)
PKG := github.com/ocean-foundation/ocean/compiler/driver
COMMIT ?= $(shell git rev-parse HEAD 2>/dev/null || echo unknown)
COMMIT_DATE ?= $(shell git log -1 --format=%cs 2>/dev/null || echo unknown)
COMMIT_YEAR ?= $(shell git log -1 --format=%cd --date=format:%Y 2>/dev/null || date -u +%Y)
BUILD_DATE ?= $(shell date -u +%Y-%m-%dT%H:%M:%SZ)
LDFLAGS := -X $(PKG).version=$(VERSION) -X $(PKG).commit=$(COMMIT) \
	-X $(PKG).commitDate=$(COMMIT_DATE) -X $(PKG).commitYear=$(COMMIT_YEAR) \
	-X $(PKG).buildDate=$(BUILD_DATE)

.PHONY: all test selfhost clean runtime
all: runtime
	mkdir -p "$(dir $(OUT))"
	"$(OCEAN)" build -ldflags "$(LDFLAGS)" -o "$(OUT)" .

# Regenerate the embedded runtime after editing compiler/runtime/*.c.
runtime:
	mkdir -p build
	"$(OCEAN)" build -o "$(EMBED)" tools/embedruntime
	"$(EMBED)"

test: all
	"$(OUT)" test ./... --notty

# Rebuild the compiler with itself.
selfhost: all
	mkdir -p "$(dir $(STAGE2))"
	"$(OUT)" build -ldflags "$(LDFLAGS)" -o "$(STAGE2)" .

clean:
	rm -rf build
