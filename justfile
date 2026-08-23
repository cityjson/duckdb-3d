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

# Run the SQL test suite against the debug build (SQL only — see test-cpp).
test-debug:
    make test_debug

# Run only the C++ kernel tests.
test-cpp:
    make test_cpp

# Stages the cityjson/spatial extensions the sqllogic runner needs, so the gated
# tests run instead of skipping. Needs network: the community download on the
# first run, and the remote Delft fixture on every run.

# Configure + build + every test, no skips.
test-full:
    {{gen}} make test_full

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

# ── PostGIS/SFCGAL differential oracle (dev-time only; NOT wired into `ci`) ──
# Regenerates test/data/postgis_oracle/golden.csv, the frozen reference values
# the CI test test/sql/postgis_oracle.test compares against. PostGIS runs offline
# here only; `make test` never needs it. See test/data/postgis_oracle/README.md.
# Defaults target Apple `container`; for Docker or rootless Podman, swap
# `--arch amd64` for `--platform linux/amd64` and set ORACLE_RUNTIME accordingly:
#
#   export ORACLE_RUNTIME=podman ORACLE_ARCH_FLAG="--platform linux/amd64"
#
# gen_golden.py reads ORACLE_RUNTIME from the environment too, so the same two
# variables cover `oracle-up`, `oracle-regen`/`oracle-reexport`, and `oracle-down`.

oracle_runtime := env_var_or_default("ORACLE_RUNTIME", "container")
oracle_container := env_var_or_default("ORACLE_CONTAINER", "pg_oracle")
oracle_image := env_var_or_default("ORACLE_IMAGE", "postgis/postgis:16-3.4")
# Apple `container` selects the amd64 image with `--arch amd64`; for Docker set
# ORACLE_RUNTIME=docker ORACLE_ARCH_FLAG="--platform linux/amd64".
oracle_arch_flag := env_var_or_default("ORACLE_ARCH_FLAG", "--arch amd64")

# The image starts a temporary server for initdb (which passes pg_isready) then
# restarts the real one, so wait for the SECOND "ready to accept connections"
# log line to avoid connecting during the init transient; fail if it never comes.

# Start the PostGIS+SFCGAL oracle container and wait until it is ready.
oracle-up:
    {{oracle_runtime}} rm -f {{oracle_container}} 2>/dev/null || true
    {{oracle_runtime}} run -d --name {{oracle_container}} {{oracle_arch_flag}} -e POSTGRES_HOST_AUTH_METHOD=trust {{oracle_image}}
    for i in $(seq 1 90); do [ "$({{oracle_runtime}} logs {{oracle_container}} 2>&1 | grep -c 'ready to accept connections')" -ge 2 ] && break; sleep 1; done
    [ "$({{oracle_runtime}} logs {{oracle_container}} 2>&1 | grep -c 'ready to accept connections')" -ge 2 ] || { echo "oracle failed to become ready in 90s" >&2; {{oracle_runtime}} logs {{oracle_container}} 2>&1 | tail -20 >&2; exit 1; }
    @echo "oracle ready ({{oracle_image}})"

# Stop and remove the oracle container.
oracle-down:
    {{oracle_runtime}} rm -f {{oracle_container}} 2>/dev/null || true

# Refresh golden.csv from the FROZEN WKB (needs `oracle-up`; no DuckDB/cityjson).
oracle-regen:
    python3 scripts/oracle/gen_golden.py

# Needs a LOCAL duckdb-cityjson build (see CITYJSON_EXTENSION in gen_golden.py);
# use only when the fixture test/data/3dbag.city.jsonl or the input set changes.

# Re-derive WKB from the fixture, then refresh the oracle values.
oracle-reexport:
    python3 scripts/oracle/gen_golden.py --reexport
