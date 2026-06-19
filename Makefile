PROJ_DIR := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))

# Configuration of extension
EXT_NAME=three_d
EXT_CONFIG=${PROJ_DIR}extension_config.cmake

# Include the Makefile from extension-ci-tools
include extension-ci-tools/makefiles/duckdb_extension.Makefile

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

.PHONY: test_cpp test_all

# Configure (idempotent), build, and run the C++ kernel tests. Omitting -G lets
# CMake reuse an existing generator or pick the platform default on a fresh dir.
test_cpp:
	cmake -S test/cpp -B $(CPP_TEST_DIR)
	cmake --build $(CPP_TEST_DIR)
	$(CPP_TEST_BIN)

# Run the full suite: SQL extension tests (debug) and the C++ kernel tests.
test_all: test_debug test_cpp
