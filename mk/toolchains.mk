# The toolchains the clients need, pinned and fetched by make (docs/details/VENDORS.md
# §2, "Toolchains"): Emscripten for the web client, llvm-mingw for the Windows
# clients. Each is a SHA-checked download under third_party/ made by its script
# on first use, so the full build needs nothing installed beyond cc, cmake,
# python3, git, curl, xz, bzip2 and nasm (libvpx's x86 assembly), and for the
# arm64 daemon Ubuntu's cross compiler. The directories go first on PATH for
# every recipe and script, so a machine-wide install is never picked up by
# accident; a bump is a version and a sum in the script.
EMSDK_VERSION      := 6.0.13
LLVM_MINGW_VERSION := 20261020

EMSDK_DIR      := third_party/emsdk-$(EMSDK_VERSION)
LLVM_MINGW_DIR := third_party/llvm-mingw-$(LLVM_MINGW_VERSION)
EMCC     := $(EMSDK_DIR)/upstream/emscripten/emcc
NODE     := $(EMSDK_DIR)/node-bin/node
MINGW_CC := $(LLVM_MINGW_DIR)/bin/x86_64-w64-mingw32-gcc

export PATH := $(CURDIR)/$(LLVM_MINGW_DIR)/bin:$(CURDIR)/$(EMSDK_DIR)/upstream/emscripten:$(CURDIR)/$(EMSDK_DIR)/node-bin:$(PATH)

# --- The release compiler and the cross build -----------------------------------
# `make RELEASE=1` (the host's architecture) compiles with gcc 12 (gcc-12 and
# g++-12), and `make ARCH=<other>` with Ubuntu's gcc 12 cross compiler for it
# (<arch>-linux-gnu-gcc-12 and -g++-12, from gcc-12-aarch64-linux-gnu,
# g++-12-aarch64-linux-gnu and libc6-dev-arm64-cross), which the daemon's rules
# and every vendor script then take as CC, CXX and AR. gcc 12, not Ubuntu
# 22.04's default gcc 11: gcc 11 does not vectorise the quantised dot products
# (nnkit/nn_ops.c), and read-aloud runs about five times slower than real time
# built with it, against about real time with gcc 12 (docs/details/TTSKIT.md §5b).
# The glibc floor is the build machine's: the release builds on Ubuntu 22.04,
# glibc 2.35
# (docs/ARCHITECTURE.md, ARCH-20; scripts/test/full/glibc_floor.sh). A cross
# build keeps its own copies of everything built: the vendored libraries in
# third_party/<name>-<arch>/ (the scripts read OC_ARCH), the objects and the
# daemon under build/<arch>/.
ifneq ($(ARCH)-$(RELEASE),$(HOST_ARCH)-0)
RELEASE_GCC := 12
ifeq ($(ARCH),$(HOST_ARCH))
override CC := gcc-$(RELEASE_GCC)
CXX         := g++-$(RELEASE_GCC)
else
override CC := $(ARCH)-linux-gnu-gcc-$(RELEASE_GCC)
CXX         := $(ARCH)-linux-gnu-g++-$(RELEASE_GCC)
AR          := $(ARCH)-linux-gnu-ar
export AR
endif
export CC CXX
# Stripped at the link: the release ships no symbols, and the host's strip
# cannot read another architecture's ELF.
override LDFLAGS += -Wl,--strip-all
endif
ifneq ($(ARCH),$(HOST_ARCH))
export OC_ARCH := $(ARCH)
ARCH_SUFFIX    := -$(ARCH)
BUILD_SUFFIX   := /$(ARCH)
endif

$(EMCC): scripts/build/emsdk.sh
	scripts/build/emsdk.sh
	@touch -c $@
$(MINGW_CC): scripts/build/llvm_mingw.sh
	scripts/build/llvm_mingw.sh
	@touch -c $@
