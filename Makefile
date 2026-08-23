PROJ_DIR := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))

# Configuration of extension
EXT_NAME=three_d
EXT_CONFIG=${PROJ_DIR}extension_config.cmake

# Include the Makefile from extension-ci-tools
include extension-ci-tools/makefiles/duckdb_extension.Makefile

# The SQL test suite depends on the st_aswkb* fixture generators, which are
# only registered when this is set (see src/functions/fixtures.cpp). Exported
# so `make test` / `make test_debug` / `make test_all` pass it to the runner.
export THREE_D_TEST_FIXTURES=1

# ──────────────────────────────────────────────────────────────────────────
# Standalone C++ kernel unit tests (Catch2).
#
# These cover the geometry kernel (WKB parsing, payload round-trips, validation,
# triangulation, measurements, distance) directly, without going through the SQL
# layer. They are the first line of defence per the repo's TDD guidance, so they
# must be runnable from the normal workflow — not only by hand.
# ──────────────────────────────────────────────────────────────────────────
CPP_TEST_DIR := build/cpp_tests
CPP_TEST_BIN := $(CPP_TEST_DIR)/three_d_unit_tests

# Homes for `test_full`. EXT_CACHE_HOME persists across runs so the httpfs and
# spatial downloads happen once; TEST_RUN_HOME is wiped and reseeded every run.
EXT_CACHE_HOME := $(PROJ_DIR)build/ext_cache
TEST_RUN_HOME := $(PROJ_DIR)build/test_home

# The cityjson build the gated interop tests bind against. A LOCAL build of the
# sibling duckdb-cityjson checkout — the published community extension still
# emits the pre-per-LoD `geometry` / `geometry_properties` columns, and the tests
# target `geometry_lod<X>` / `geometry_properties_lod<X>` only. Override to point
# elsewhere. See docs/CITYJSON_INTEROP.md.
CITYJSON_EXTENSION ?= $(PROJ_DIR)../duckdb-cityjson/build/release/extension/cityjson/cityjson.duckdb_extension

.PHONY: test_cpp test_all test_full

# Configure (idempotent), build, and run the C++ kernel tests. Omitting -G lets
# CMake reuse an existing generator or pick the platform default on a fresh dir.
test_cpp:
	cmake -S test/cpp -B $(CPP_TEST_DIR)
	cmake --build $(CPP_TEST_DIR)
	$(CPP_TEST_BIN)

# Run the full suite: SQL extension tests (debug) and the C++ kernel tests.
#
# Note that `test_debug` runs whatever binary build/debug already holds — the
# ci-tools target has no build dependency. Build first, or use `test_full`.
test_all: test_debug test_cpp

# ──────────────────────────────────────────────────────────────────────────
# One command, no implicit setup: configure + build + every test, with the
# cityjson/spatial-gated tests actually running instead of skipping.
#
# Runs against the RELEASE build, unlike test_debug. The gated tests load
# third-party extensions (cityjson, spatial, httpfs) that only exist as
# release-built binaries, and a debug DuckDB tracks allocations the release
# allocator does not — streaming the remote Delft tile through a release-built
# cityjson inside the debug binary aborts on
# "Assertion triggered in allocator.cpp: allocation_count >= size". Mixing the
# two flavours is not a supported configuration, so this target does not try.
# Debug-allocator coverage of everything else comes from `make test_all`.
#
# With DUCKDB_TEST_AUTOLOADING set, the sqllogic runner answers every `require
# <ext>` by running `INSTALL <ext> FROM '<local repo>'` before `LOAD <ext>` —
# including for an extension that is already statically linked. So three things
# have to hold at once, and the recipe establishes all three rather than relying
# on the invoking shell:
#
#   1. cityjson, spatial AND three_d are all present in the runner's local
#      repository, build/release/repository/<version>/<platform>. three_d is
#      linked into the binary, but the INSTALL step still has to find a file, so
#      the freshly built .duckdb_extension is staged alongside the others.
#   2. cityjson comes from $(CITYJSON_EXTENSION) — the sibling duckdb-cityjson
#      build, NOT `INSTALL cityjson FROM community`. The published community
#      build emits the pre-per-LoD column shape the interop tests no longer
#      target, so a community copy makes them fail rather than skip.
#   3. httpfs is already installed in the runner's HOME, because the remote
#      Delft test autoloads it and autoinstall resolves against the official
#      repository rather than the local one.
#
# Steps run in one recipe, not as prerequisites, so ordering survives `make -j`.
# Requires network access: the httpfs/spatial download on the first run, and the
# remote Delft fixture on every run.
# ──────────────────────────────────────────────────────────────────────────
test_full:
	@set -e; \
	if [ ! -f "$(CITYJSON_EXTENSION)" ]; then \
	  echo "!! cityjson extension not found at:"; \
	  echo "!!   $(CITYJSON_EXTENSION)"; \
	  echo "!! Build the sibling checkout — (cd ../duckdb-cityjson && GEN=ninja make release)"; \
	  echo "!! — or set CITYJSON_EXTENSION to another build. The published community"; \
	  echo "!! extension is NOT supported: it still emits the flat geometry /"; \
	  echo "!! geometry_properties columns. See docs/CITYJSON_INTEROP.md."; \
	  exit 1; \
	fi; \
	$(MAKE) release; \
	duckdb_bin=./build/release/duckdb; \
	version=$$($$duckdb_bin -noheader -list -c "SELECT version()"); \
	platform=$$($$duckdb_bin -noheader -list -c "PRAGMA platform"); \
	echo "==> staging test extensions for $$version/$$platform"; \
	mkdir -p $(EXT_CACHE_HOME); \
	HOME=$(EXT_CACHE_HOME) $$duckdb_bin -unsigned -c \
	  "INSTALL httpfs; INSTALL spatial;" || { \
	    echo "!! could not install the test extensions for $$version."; \
	    echo "!! A -dev DuckDB version has no published extensions — pin the"; \
	    echo "!! duckdb submodule to a release tag."; \
	    exit 1; \
	  }; \
	cache_dir=$(EXT_CACHE_HOME)/.duckdb/extensions/$$version/$$platform; \
	repo_dir=$(PROJ_DIR)build/release/repository/$$version/$$platform; \
	mkdir -p $$repo_dir; \
	cp $$cache_dir/spatial.duckdb_extension $$repo_dir/; \
	cp $$cache_dir/spatial.duckdb_extension.info $$repo_dir/; \
	cp "$(CITYJSON_EXTENSION)" $$repo_dir/cityjson.duckdb_extension; \
	cp build/release/extension/three_d/three_d.duckdb_extension $$repo_dir/; \
	echo "==> cityjson staged from $(CITYJSON_EXTENSION)"; \
	rm -rf $(TEST_RUN_HOME); \
	run_ext_dir=$(TEST_RUN_HOME)/.duckdb/extensions/$$version/$$platform; \
	mkdir -p $$run_ext_dir; \
	cp $$cache_dir/httpfs.duckdb_extension $$run_ext_dir/; \
	cp $$cache_dir/httpfs.duckdb_extension.info $$run_ext_dir/; \
	echo "==> SQL tests (release, gated tests enabled)"; \
	sql_log=$$(mktemp); \
	if HOME=$(TEST_RUN_HOME) DUCKDB_TEST_AUTOLOADING=available \
	     LOCAL_EXTENSION_REPO=$(PROJ_DIR)build/release/repository \
	     ./build/release/$(TEST_PATH) "$(TESTS_BASE_DIRECTORY)*" >$$sql_log 2>&1; then \
	  sql_status=0; \
	else \
	  sql_status=$$?; \
	fi; \
	cat $$sql_log; \
	if [ $$sql_status -ne 0 ]; then rm -f $$sql_log; exit $$sql_status; fi; \
	if grep -qE "skipped test|were skipped" $$sql_log; then \
	  echo "!! a test skipped under test_full: the gated extensions did not stage."; \
	  echo "!! Under this target a skip is a failure, not an expectation --"; \
	  echo "!! the cityjson/spatial interop tests are exactly the ones that skip."; \
	  rm -f $$sql_log; exit 1; \
	fi; \
	rm -f $$sql_log; \
	echo "==> C++ kernel tests"; \
	$(MAKE) test_cpp
