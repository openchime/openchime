# The full build and the full suite (docs/TESTING.md): everything a release
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
# plain then threads in test-all.yml's one job, packages, image and windows in
# release-candidate.yml's `stage`. Unset, every part in turn:
#
#   plain     the daemon with every feature, the Linux TUI, the web client, the
#             test binary, the e2e and demo clients and the fuzzing harnesses;
#             then the source checks, actionlint, the suites that start no
#             thread, and media, plainly with their timing judged, the
#             static-link check, the end-to-end run, the release's compiler,
#             the speech round trip, the web client in a headless Chrome and
#             its objects' header dependencies, the GUI smoke on the web, and
#             each fuzzing harness for its fixed inputs, one at a time, inside
#             FUZZ_SECONDS (20) in all
#   threads   the test binary under ThreadSanitizer; then the suites that start
#             threads, under it, their one run in the full suite
#   packages  the release's daemon for ARCH (zig against glibc 2.34) with its
#             data, and the .deb, .rpm and tarball from it under PKG_VERSION, in
#             DIST; then the glibc floor, one binary in all three, speech, the
#             .deb installed and speaking, and its unit file
#   image     the image IMAGE, laid from the tree IMAGE_TREE (a tarball
#             packaging/image-context.sh takes) for PKG_ARCH; then it starts,
#             and its database replicates and restores
#   windows   the Windows GUI and TUI, and the installer and MSIX from them
#             under PKG_VERSION, in WIN_DIST; then their version resources,
#             the installer and the MSIX
#
# A check on a binary or an image this machine cannot run says so and skips
# it (the arm64 package and image, on an x86 runner).
FULL_SWITCHES := TTS=1 STT=1 SUM=1 WEB=1 REMEMBER=0
FULL_PARTS    := plain threads packages image windows
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
build-all-plain:
	+@$(MAKE) --no-print-directory $(FULL_SWITCHES) daemon linux $(WEB_DIR)/openchime.js \
	    $(TEST_BIN) build/e2e_client build/demo_client build/signin $(patsubst %,build/fuzz/%,$(FUZZ_HARNESSES))
test-all-plain:
	scripts/test/fast/source.sh
	scripts/test/fast/actionlint.sh
	scripts/test/full/suites.sh
	scripts/test/full/static_link.sh $(BIN)
	scripts/test/full/e2e.sh
	+MAKE="$(MAKE) $(FULL_SWITCHES)" scripts/test/full/release_cc.sh
	scripts/test/full/speech.sh ./$(BIN)
	NODE="$(NODE)" scripts/test/full/web.sh
	+MAKE="$(MAKE) $(FULL_SWITCHES)" scripts/test/full/web_deps.sh
	NODE="$(NODE)" scripts/test/full/gui_web_smoke.sh
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
