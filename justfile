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

# Stages the LOCAL cityjson build plus spatial/three_d for the sqllogic runner,
# so the gated tests run instead of skipping. Runs against the release build (see
# the note in the Makefile). Needs network: the httpfs/spatial download on the
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

# Path to the cityjson extension used by shell-cityjson and `make test_full`.
# A LOCAL duckdb-cityjson build: the community-published one emits the stale flat
# `geometry` column shape (see docs/CITYJSON_INTEROP.md).
cityjson_extension := env_var_or_default("CITYJSON_EXTENSION", "../duckdb-cityjson/build/release/extension/cityjson/cityjson.duckdb_extension")

# Launch the shell with the local cityjson + three_d loaded.
shell-cityjson:
    ./build/release/duckdb -unsigned -cmd "LOAD '{{cityjson_extension}}'; LOAD three_d;"

# ── DuckDB-Wasm ──

# Wasm toolchain pins, mirroring the CI distribution pipeline.
# emsdk: extension-ci-tools v1.5.4 `_extension_distribution.yml` pins
# emscripten-core/setup-emsdk@v13 with version 3.1.71.
# vcpkg baseline: the `builtin-baseline` commit in vcpkg.json — a plain shallow clone
# does NOT contain it, so `wasm-setup` fetches it explicitly (manifest resolution
# fails otherwise).
emsdk_version := "3.1.71"
vcpkg_baseline := "84bab45d415d22042bd0b9081aea57f362da3f35"

# Installs the pinned emsdk and a durable vcpkg checkout under the gitignored
# .vendor/. Idempotent — safe to re-run. ~2 GB and ~10 min the first time.

# One-time toolchain bootstrap for `just wasm` (emsdk + vcpkg into .vendor/).
wasm-setup:
    #!/usr/bin/env bash
    set -euo pipefail
    mkdir -p .vendor
    if [ ! -d .vendor/emsdk ]; then
        git clone --depth 200 https://github.com/emscripten-core/emsdk .vendor/emsdk
    fi
    (cd .vendor/emsdk && ./emsdk install {{emsdk_version}} && ./emsdk activate {{emsdk_version}})
    # A blobless partial clone, NOT a shallow one. vcpkg resolves `proj` through its
    # versions database to a specific *port tree* — 62e9ace for 9.4.0 — and a shallow
    # clone contains no history to find it in, so the install dies with "failed to
    # unpack tree object ... vcpkg was cloned as a shallow repository". `--filter`
    # keeps every commit and tree while fetching blobs on demand, which resolves any
    # port version at a fraction of a full clone's size.
    if [ ! -d .vendor/vcpkg ]; then
        git clone --filter=blob:none https://github.com/microsoft/vcpkg .vendor/vcpkg
        (cd .vendor/vcpkg && ./bootstrap-vcpkg.sh -disableMetrics)
    fi

# Build the DuckDB-Wasm extension. Run `just wasm-setup` once first. The first build
# is slow — vcpkg compiles PROJ and its dependencies for wasm32-emscripten — and
# later builds reuse those binaries. Output lands in
# build/<flavour>/extension/three_d/three_d.duckdb_extension.wasm, and the build also
# writes a loadable extension repository at build/<flavour>/repository/.
#
# The default is `wasm_eh`, not `wasm_mvp` as in the sibling duckdb-cityjson repo,
# because the consumers differ: that repo's default serves its wasm_mvp smoke
# harness, whereas here a browser is the only consumer. duckdb-wasm's selectBundle()
# picks the `eh` bundle wherever native wasm exceptions are available, and an `eh`
# instance can only load `eh` extensions.
#
# NOTE: no ninja here, unlike every other build recipe. The wasm targets in
# extension-ci-tools' duckdb_extension.Makefile hardcode
# `emmake make -j8 -Cbuild/<flavour>`, so a Ninja-generated tree has no makefile and
# the build dies with "No targets specified and no makefile found". Recovering also
# needs `rm -rf build/<flavour>` — CMake refuses to switch generator in place.
#
# ST_3DTransform calls proj_create_crs_to_crs, which reads PROJ's EPSG database at
# runtime. That database is not in the Emscripten filesystem, so expect that one
# function to fail under wasm; the other ST_3D* functions do not touch it.

# Build the DuckDB-Wasm extension (eh by default) with the pinned emsdk + .vendor/vcpkg.
wasm flavour="wasm_eh":
    #!/usr/bin/env bash
    set -euo pipefail
    case "{{flavour}}" in
        wasm_mvp|wasm_eh|wasm_threads) ;;
        *) echo "unknown wasm flavour: {{flavour}} (want wasm_mvp, wasm_eh or wasm_threads)" >&2; exit 2 ;;
    esac
    source .vendor/emsdk/emsdk_env.sh
    VCPKG_TOOLCHAIN_PATH="$(pwd)/.vendor/vcpkg/scripts/buildsystems/vcpkg.cmake" make {{flavour}}
    echo "-> build/{{flavour}}/extension/three_d/three_d.duckdb_extension.wasm"

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
