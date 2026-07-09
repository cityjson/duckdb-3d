# duckdb-3d developer shortcuts.
# Run `just` (or `just --list`) to see all recipes.

set shell := ["bash", "-uc"]

# Use ninja for faster incremental builds when it is installed.
gen := `command -v ninja >/dev/null 2>&1 && echo "GEN=ninja" || echo ""`

# List available recipes (default).
default:
    @just --list

# Build the release extension (build/release/duckdb has three_d preloaded).
build:
    {{gen}} make

# Build the debug extension.
build-debug:
    {{gen}} make debug

# Run the SQL test suite against the release build.
test:
    make test

# Run SQL + C++ tests against the debug build.
test-debug:
    make test_debug

# Run only the C++ kernel tests.
test-cpp:
    make test_cpp

# Auto-format C++ sources in place (clang-format).
format:
    make format-fix

# Check formatting without modifying files (same as CI's Format Check).
format-check:
    make format-check

# Run clang-tidy (same as CI's Tidy Check). Heavy: builds DuckDB with tidy enabled.
tidy:
    make tidy-check

# Local CI gate: format check + build + full test suite (run `just tidy` for the tidy pass).
ci: format-check build test test-cpp
    @echo "✅ local CI gate passed (run 'just tidy' for the clang-tidy pass)"

# Launch the DuckDB shell with three_d preloaded.
shell:
    ./build/release/duckdb

# Launch the shell with cityjson + three_d loaded (needs `INSTALL cityjson FROM community` once).
shell-cityjson:
    ./build/release/duckdb -unsigned -cmd "LOAD cityjson; LOAD three_d;"

# Remove build artifacts.
clean:
    make clean
