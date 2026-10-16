# Radiance4D
#
# The presets do the real work; these targets name the combinations worth
# running. `make check` is the gate: what CI runs, minus the GPU tests a hosted
# runner has no device for.

PRESET      ?= windows-d3d12
CONFIG      ?= Debug
BUILD_DIR   := build/$(PRESET)
JOBS        ?= 16

# Visual Studio ships clang-format 19; PATH usually does not have it. CI pins
# the same version, because clang-format releases disagree about formatting.
CLANG_FORMAT ?= $(shell command -v clang-format 2>/dev/null || \
	echo "/c/Program Files/Microsoft Visual Studio/2022/Community/VC/Tools/Llvm/bin/clang-format.exe")
PYTHON      ?= python

SOURCES := $(shell git ls-files 'include/*.hpp' 'src/*.cpp' 'src/*.hpp' \
	'app/*.cpp' 'app/*.hpp' 'tests/*.cpp' 'tests/*.hpp')

.PHONY: help
help:
	@echo "configure      cmake --preset $(PRESET)"
	@echo "build          build $(CONFIG) (CONFIG=Debug|RelWithDebInfo|Release)"
	@echo "test           ctest, everything"
	@echo "test-unit      ctest, no GPU and no device tests"
	@echo "test-ci        ctest as CI runs it: no GPU label"
	@echo "format         rewrite the sources with clang-format"
	@echo "format-check   fail if any source is unformatted"
	@echo "check          format-check + build + test-ci, the gate"
	@echo "bench          the 1080p splat benchmark (needs SPLATS=<file.ply>)"
	@echo "package        stage a runnable release tree under dist/"
	@echo "clean          remove the build directory"

.PHONY: configure
configure:
	cmake --preset $(PRESET)

.PHONY: build
build: configure
	cmake --build $(BUILD_DIR) --config $(CONFIG) --parallel $(JOBS)

.PHONY: test
test:
	ctest --test-dir $(BUILD_DIR) -C $(CONFIG) -j $(JOBS) --output-on-failure

.PHONY: test-unit
test-unit:
	ctest --test-dir $(BUILD_DIR) -C $(CONFIG) -j $(JOBS) -L unit --output-on-failure

# A hosted runner has no discrete GPU; the device tests use the pinned WARP.
.PHONY: test-ci
test-ci:
	ctest --test-dir $(BUILD_DIR) -C $(CONFIG) -j $(JOBS) \
		--label-exclude gpu --output-on-failure

.PHONY: format
format:
	@"$(CLANG_FORMAT)" -i $(SOURCES)

.PHONY: format-check
format-check:
	@fail=0; for f in $(SOURCES); do \
		"$(CLANG_FORMAT)" "$$f" | diff -q - "$$f" >/dev/null || { echo "unformatted: $$f"; fail=1; }; \
	done; \
	if [ $$fail -ne 0 ]; then echo "run: make format"; exit 1; fi; \
	echo "formatting clean"

.PHONY: check
check: format-check build test-ci

.PHONY: bench
bench:
	@test -n "$(SPLATS)" || { echo "set SPLATS=<path to point_cloud.ply>"; exit 1; }
	$(BUILD_DIR)/bin/RelWithDebInfo/radiance4d_viewer.exe \
		--splat-ply "$(SPLATS)" --width 1920 --height 1080 --benchmark 200

# Everything needed to run on a machine without the repository: the executable,
# the compiled shaders, and the Agility SDK and WARP the loader expects beside it.
.PHONY: package
package:
	$(MAKE) build CONFIG=RelWithDebInfo
	rm -rf dist
	mkdir -p dist
	cp -r $(BUILD_DIR)/bin/RelWithDebInfo/. dist/
	cp LICENSE README.md dist/
	@echo "staged dist/ -- zip it for a release"

.PHONY: clean
clean:
	rm -rf $(BUILD_DIR) dist
