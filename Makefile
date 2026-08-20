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

# Homes for `test_full`. EXT_CACHE_HOME persists across runs so the community
# download happens once; TEST_RUN_HOME is wiped and reseeded every run, which is
# what keeps `require cityjson` off the "origin is different" path.
EXT_CACHE_HOME := $(PROJ_DIR)build/ext_cache
TEST_RUN_HOME := $(PROJ_DIR)build/test_home

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
# `require cityjson` / `require spatial` need three things to hold at once, and
# the recipe establishes all three rather than relying on the invoking shell:
#
#   1. The extension binaries are discoverable by the sqllogic runner, which
#      looks in build/<flavour>/repository/<version>/<platform>.
#   2. The runner's HOME has no conflicting copy of them — DuckDB refuses to
#      re-INSTALL an extension whose origin differs from the installed one.
#   3. httpfs is already installed in that HOME, because the remote Delft test
#      autoloads it and autoinstall resolves against the official repository
#      rather than the local one.
#
# Steps run in one recipe, not as prerequisites, so ordering survives `make -j`.
# Requires network access: the community download on the first run, and the
# remote Delft fixture on every run.
# ──────────────────────────────────────────────────────────────────────────
test_full:
	@set -e; \
	$(MAKE) debug; \
	duckdb_bin=./build/debug/duckdb; \
	version=$$($$duckdb_bin -noheader -list -c "SELECT version()"); \
	platform=$$($$duckdb_bin -noheader -list -c "PRAGMA platform"); \
	echo "==> staging test extensions for $$version/$$platform"; \
	mkdir -p $(EXT_CACHE_HOME); \
	HOME=$(EXT_CACHE_HOME) $$duckdb_bin -unsigned -c \
	  "INSTALL httpfs; INSTALL spatial; INSTALL cityjson FROM community;" || { \
	    echo "!! could not install the test extensions for $$version."; \
	    echo "!! A -dev DuckDB version has no published extensions — pin the"; \
	    echo "!! duckdb submodule to a release tag."; \
	    exit 1; \
	  }; \
	cache_dir=$(EXT_CACHE_HOME)/.duckdb/extensions/$$version/$$platform; \
	repo_dir=build/debug/repository/$$version/$$platform; \
	mkdir -p $$repo_dir; \
	for ext in cityjson spatial; do \
	  cp $$cache_dir/$$ext.duckdb_extension $$repo_dir/; \
	  cp $$cache_dir/$$ext.duckdb_extension.info $$repo_dir/; \
	done; \
	rm -rf $(TEST_RUN_HOME); \
	run_ext_dir=$(TEST_RUN_HOME)/.duckdb/extensions/$$version/$$platform; \
	mkdir -p $$run_ext_dir; \
	cp $$cache_dir/httpfs.duckdb_extension $$run_ext_dir/; \
	cp $$cache_dir/httpfs.duckdb_extension.info $$run_ext_dir/; \
	echo "==> SQL tests (debug, gated tests enabled)"; \
	sql_log=$$(mktemp); \
	if HOME=$(TEST_RUN_HOME) DUCKDB_TEST_AUTOLOADING=available \
	     ./build/debug/$(TEST_PATH) "$(TESTS_BASE_DIRECTORY)*" >$$sql_log 2>&1; then \
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
