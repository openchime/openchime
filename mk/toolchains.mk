# The toolchains, pinned and fetched by make (docs/VENDORS.md §2, "Toolchains"):
# zig for the release's Linux compiler and the arm64 cross-build, Emscripten
# for the web client, llvm-mingw for the Windows clients. Each is a SHA-checked
# download under third_party/ made by its script on first use, so `make full`
# needs nothing installed beyond cc, cmake, python3, git, curl, xz, bzip2 and
# nasm (libvpx's x86 assembly). The directories go first on PATH for every
# recipe and script, so a machine-wide install is never picked up by accident;
# a bump is a version and a sum in the script.
ZIG_VERSION        := 0.13.0
EMSDK_VERSION      := 6.0.8
LLVM_MINGW_VERSION := 20261006

ZIG_DIR        := third_party/zig-$(ZIG_VERSION)
EMSDK_DIR      := third_party/emsdk-$(EMSDK_VERSION)
LLVM_MINGW_DIR := third_party/llvm-mingw-$(LLVM_MINGW_VERSION)
ZIG      := $(ZIG_DIR)/zig
EMCC     := $(EMSDK_DIR)/upstream/emscripten/emcc
NODE     := $(EMSDK_DIR)/node-bin/node
MINGW_CC := $(LLVM_MINGW_DIR)/bin/x86_64-w64-mingw32-gcc

export PATH := $(CURDIR)/$(LLVM_MINGW_DIR)/bin:$(CURDIR)/$(EMSDK_DIR)/upstream/emscripten:$(CURDIR)/$(EMSDK_DIR)/node-bin:$(CURDIR)/$(ZIG_DIR):$(PATH)

# --- The release compiler and the cross build -----------------------------------
# `make RELEASE=1` (the host's architecture) and `make ARCH=<other>` compile
# with zig's clang against glibc 2.34 through scripts/release/zig_cc.sh, which
# the daemon's rules and every vendor script then take as CC and CXX. A cross
# build keeps its own copies of everything built: the vendored libraries in
# third_party/<name>-<arch>/ (the scripts read OC_ARCH), the objects and the
# daemon under build/<arch>/.
ifneq ($(ARCH)-$(RELEASE),$(HOST_ARCH)-0)
TARGET      := $(ARCH)-linux-gnu.2.34
CC_WRAP     := build/cc-$(ARCH)
CXX_WRAP    := build/cxx-$(ARCH)
override CC := $(CURDIR)/$(CC_WRAP)
CXX         := $(CURDIR)/$(CXX_WRAP)
export CC CXX
# ONNX Runtime's AVX-512 kernels enable the instructions per file and need
# zig's clang told that 512-bit vectors are allowed; nothing else notices.
ZIG_EXTRA   := $(if $(filter x86_64,$(ARCH)),-mevex512,)
$(CC_WRAP) $(CXX_WRAP) &: $(ZIG) scripts/release/zig_cc.sh | build
	@printf '#!/bin/sh\nexec %s/scripts/release/zig_cc.sh cc %s %s -- "$$@"\n' "$(CURDIR)" "$(TARGET)" "$(ZIG_EXTRA)" > $(CC_WRAP)
	@printf '#!/bin/sh\nexec %s/scripts/release/zig_cc.sh c++ %s %s -- "$$@"\n' "$(CURDIR)" "$(TARGET)" "$(ZIG_EXTRA)" > $(CXX_WRAP)
	@chmod +x $(CC_WRAP) $(CXX_WRAP)
TOOLCHAIN_DEPS := $(CC_WRAP) $(CXX_WRAP)
TTS_CXXLIB     := -lc++
# Stripped at the link: the release ships no symbols, and the host's strip
# cannot read another architecture's ELF.
override LDFLAGS += -Wl,--strip-all
endif
ifneq ($(ARCH),$(HOST_ARCH))
export OC_ARCH := $(ARCH)
ARCH_SUFFIX    := -$(ARCH)
BUILD_SUFFIX   := /$(ARCH)
endif

$(ZIG): scripts/build/zig.sh
	scripts/build/zig.sh
	@touch -c $@
$(EMCC): scripts/build/emsdk.sh
	scripts/build/emsdk.sh
	@touch -c $@
$(MINGW_CC): scripts/build/llvm_mingw.sh
	scripts/build/llvm_mingw.sh
	@touch -c $@
