# The full build and the full suite (docs/details/TESTING.md): everything a release
# ships, built and checked, in CI only, once per release-candidate
# (release-candidate.yml, which calls test-all.yml for the code tests). The
# full suite is a superset of the fast one (mk/tests.mk): every suite, the
# fast ones included, and every source check.
#
#   make build-all [PART=<part>]   builds
#   make test-all  [PART=<part>]   checks what build-all built, with the same
#                                  variables; it builds nothing a release ships
#
# PART only splits the work into the steps CI runs, one part each, in turn:
# plain, threads and linux-client in test-all.yml's one job, packages, image
# and windows in release-candidate.yml's `stage`. Unset, every part in turn:
#
#   plain     the daemon with every feature, the Linux TUI, the Linux GUI, the
#             web client, the test binary, the e2e and demo clients, the
#             fuzzing harnesses, and sdltext's and the Linux GUI's test
#             programs; then
#             the source checks, actionlint, the suites that start no thread,
#             and media, plainly with their timing judged, the static-link
#             check, the end-to-end run, the speech round trip, sdltext on
#             FreeType, the Linux GUI's test program on a session bus of its
#             own, the web client's objects' header dependencies, and each
#             fuzzing harness for its fixed inputs, one at a time, inside
#             FUZZ_SECONDS (20) in all
#   threads   the test binary under ThreadSanitizer; then the suites that start
#             threads, under it, their one run in the full suite
#   packages  the release's daemon for ARCH (gcc 12, glibc 2.35) with its
#             data, and the .deb, .rpm and tarball from it under PKG_VERSION, in
#             DIST; then the glibc floor, one binary in all three, speech, the
#             .deb installed and speaking, and its unit file
#   image     the image IMAGE, laid from the tree IMAGE_TREE (a tarball
#             packaging/image-context.sh takes) for PKG_ARCH; then it starts,
#             and its database replicates and restores
#   windows   the Windows GUI and TUI, and the installer and MSIX from them
#             under PKG_VERSION, in WIN_DIST; then their version resources,
#             the installer and the MSIX
#   linux-client
#             the Linux GUI for ARCH as the release builds it (gcc 12), twice:
#             against Ubuntu 24.04's libraries (the machine's for x86_64, the
#             pinned arm64 sysroot's for aarch64), packaged as the .deb and
#             the .rpm, and against the pinned Freedesktop SDK, assembled as
#             the Flatpak, all under PKG_VERSION, in DIST; then the desktop
#             entry and the metainfo validate, the packages hold the client
#             for ARCH with the dependencies they declare, the .deb's client
#             says its version where it can run, and the Flatpak names its
#             runtime and its sandbox (scripts/test/full/linux_client.sh)
#
# A check on a binary or an image this machine cannot run says so and skips
# it (the arm64 package and image, on an x86 runner).
FULL_SWITCHES := TTS=1 STT=1 SUM=1 WEB=1 EXPORT=1 CALLREC=1 REMEMBER=0
FULL_PARTS    := plain threads packages image windows linux-client
PKG_ARCH    ?= $(if $(filter aarch64,$(ARCH)),arm64,amd64)
PKG_VERSION ?= 0
DIST        ?= dist/linux-$(PKG_ARCH)
WIN_BIN     := build
WIN_DIST    ?= dist/windows-x64
FULL_RUN     = $(or $(PART),$(FULL_PARTS))

# Each part in turn, in a make of its own; refused outside CI.
define FULL_PARTS_RUN
	@scripts/test/lib/require_ci.sh $@
	@[ -z "$(filter-out $(FULL_PARTS),$(FULL_RUN))" ] || { echo "make $@: PART is one of: $(FULL_PARTS)" >&2; exit 2; }
	+@$(foreach p,$(FULL_RUN),$(MAKE) --no-print-directory $@-$(p) &&) true
endef
build-all:
	$(FULL_PARTS_RUN)
test-all:
	$(FULL_PARTS_RUN)

# --- plain --------------------------------------------------------------------
# The Linux GUI and its test programs need system development packages
# (mk/linux.mk), which test-all.yml installs; built here, in the full build
# only, since `make build` must succeed on a host without them.
build-all-plain:
	+@$(MAKE) --no-print-directory $(FULL_SWITCHES) daemon linux linux-gui $(WEB_DIR)/openchime.js \
	    $(TEST_BIN) build/e2e_client build/demo_client $(patsubst %,build/fuzz/%,$(FUZZ_HARNESSES)) \
	    build/sdltext_test build/linux_gui_test
test-all-plain:
	scripts/test/fast/source.sh
	scripts/test/fast/actionlint.sh
	scripts/test/full/suites.sh
	scripts/test/full/static_link.sh $(BIN)
	scripts/test/full/e2e.sh
	scripts/test/full/speech.sh ./$(BIN)
	scripts/test/full/sdltext.sh
	scripts/test/full/linux_gui.sh
	+MAKE="$(MAKE) $(FULL_SWITCHES)" scripts/test/full/web_deps.sh
	FUZZ_HARNESSES="$(foreach h,$(FUZZ_HARNESSES),$(h):$(FUZZ_DICT_$(h)):$(FUZZ_RUNS_$(h)))" scripts/test/full/fuzz.sh

# --- threads ------------------------------------------------------------------
build-all-threads:
	+@$(MAKE) --no-print-directory $(TSAN_BIN)
test-all-threads:
	scripts/test/full/tsan.sh

# --- packages -----------------------------------------------------------------
# One binary per architecture feeds the .deb, the .rpm, the tarball and the
# image: the daemon as the release builds it (RELEASE=1; ARCH=aarch64
# cross-builds arm64's), with its data and the web client's wasm beside it.
build-all-packages:
	+@$(MAKE) --no-print-directory RELEASE=1 $(FULL_SWITCHES) daemon
	rm -rf $(DIST) && mkdir -p $(DIST)
	cp $(BIN) $(DIST)/openchimed
	cp -r tts stt web $(DIST)/
	packaging/build-rpm.sh     $(PKG_VERSION) $(PKG_ARCH) $(DIST)/openchimed $(DIST) >/dev/null
	packaging/build-deb.sh     $(PKG_VERSION) $(PKG_ARCH) $(DIST)/openchimed $(DIST)
	packaging/build-tarball.sh $(PKG_VERSION) $(PKG_ARCH) $(DIST)/openchimed $(DIST)
test-all-packages:
	scripts/test/full/glibc_floor.sh $(DIST)/openchimed
	scripts/test/full/same_binary.sh $(DIST) $(PKG_VERSION) $(PKG_ARCH)
	scripts/test/full/speech.sh $(DIST)/openchimed
	scripts/test/full/package.sh $(DIST) $(PKG_VERSION) $(PKG_ARCH)
	scripts/test/full/unit_file.sh $(DIST)/openchimed

# --- image --------------------------------------------------------------------
# Built with buildah, not docker: the packaged tree laid onto a glibc base
# (Dockerfile), no compile inside, so an arm64 image comes off an x86 runner.
build-all-image:
	@[ -n "$(IMAGE)" ] && [ -n "$(IMAGE_TREE)" ] || { echo "make build-all PART=image: IMAGE and IMAGE_TREE name the image and the tree" >&2; exit 2; }
	packaging/image-context.sh $(IMAGE_TREE) image
	buildah build --arch $(PKG_ARCH) -f Dockerfile -t $(IMAGE) image
test-all-image:
	@[ -n "$(IMAGE)" ] || { echo "make test-all PART=image: IMAGE names the image" >&2; exit 2; }
	scripts/test/full/image.sh $(IMAGE)
	scripts/test/full/db_restore.sh $(IMAGE)

# --- windows ------------------------------------------------------------------
# Cross-compiled as the fast build does, then packaged on Linux: NSIS
# (makensis) and makemsix (packaging/build-windows.sh), unsigned.
build-all-windows:
	+@$(MAKE) --no-print-directory windows
	packaging/build-windows.sh $(PKG_VERSION) $(WIN_BIN) $(WIN_DIST)
test-all-windows:
	scripts/test/full/windows.sh $(WIN_BIN) $(WIN_DIST) $(PKG_VERSION)

# --- linux-client -------------------------------------------------------------
# The Linux client's packages for ARCH, built on Ubuntu 24.04 (ARCH-20): the
# .deb and the .rpm carry one binary, built against Ubuntu 24.04's libraries,
# and the Flatpak another, built against the SDK of the runtime it runs on
# (mk/linux.mk, "Sysroots"). aarch64 is cross-compiled, with no emulation:
# the arm64 libraries are a sysroot's, never packages installed on the
# machine. Only this part's own files in DIST are replaced, so it can share
# DIST with the packages part, and with the other architecture's.
LINUX_CLIENT_MAKE    = $(MAKE) --no-print-directory RELEASE=1 REMEMBER=0
LINUX_CLIENT_DEB_BIN = $(LINUX_GUI_BIN)
LINUX_CLIENT_FP_BIN  = build$(BUILD_SUFFIX)/$(notdir $(FLATPAK_SYSROOT))/openchime-gui
LINUX_CLIENT_RPM_ARCH := $(if $(filter aarch64,$(ARCH)),aarch64,x86_64)
build-all-linux-client:
	+@$(LINUX_CLIENT_MAKE) linux-gui
	+@$(LINUX_CLIENT_MAKE) LINUX_SYSROOT=$(FLATPAK_SYSROOT) linux-gui
	mkdir -p $(DIST)
	rm -rf $(DIST)/openchime_$(PKG_VERSION)_$(PKG_ARCH).deb $(DIST)/openchime-$(PKG_VERSION)-1.$(LINUX_CLIENT_RPM_ARCH).rpm \
	    $(DIST)/io.openchime.OpenChime-$(PKG_VERSION)-$(ARCH).flatpak
	packaging/build-client-rpm.sh     $(PKG_VERSION) $(PKG_ARCH) $(LINUX_CLIENT_DEB_BIN) $(DIST) >/dev/null
	packaging/build-client-deb.sh     $(PKG_VERSION) $(PKG_ARCH) $(LINUX_CLIENT_DEB_BIN) $(DIST)
	packaging/build-client-flatpak.sh $(PKG_VERSION) $(ARCH) $(LINUX_CLIENT_FP_BIN) $(DIST)
test-all-linux-client:
	scripts/test/full/linux_client.sh $(DIST) $(PKG_VERSION) $(ARCH)
