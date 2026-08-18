# Building & Developing `duckdb-3d`

Detailed build, test, and distribution notes for contributors. For the project overview see
the [top-level README](../README.md); for the function reference see
[FUNCTIONS.md](./FUNCTIONS.md).

## Prerequisites

- A C++17 toolchain and CMake (the DuckDB extension build handles the rest).
- The `duckdb` and `extension-ci-tools` git submodules. Clone with
  `git clone --recurse-submodules …`, or initialise after the fact:
  ```sh
  git submodule update --init --recursive
  ```
- Recommended: [ccache](https://ccache.dev) and [ninja](https://ninja-build.org) for fast
  incremental builds — DuckDB itself is compiled on the first build, and these let you avoid
  recompiling it every time.

The extension targets DuckDB **`v1.5.x`**.

## Build

```sh
make                 # release build
GEN=ninja make       # faster, with ninja + ccache installed
make debug           # debug build (build/debug/…)
```

Main artifacts:

```
build/release/duckdb                                          # shell with three_d preloaded
build/release/test/unittest                                  # test runner (extension linked in)
build/release/extension/three_d/three_d.duckdb_extension     # loadable extension
```

`extension_config.cmake` declares the `three_d` target with `LOAD_TESTS`; add extra
extensions to build alongside it there (for example `duckdb_extension_load(json)`).

## Run

```sh
./build/release/duckdb        # three_d is already loaded
```

```sql
D SELECT ST_3DGeometryType(ST_Geom3DFromWKB(wkb)) FROM …;
```

To load a distributed binary into a stock DuckDB, start it with unsigned extensions allowed
and `LOAD` the file:

```sh
duckdb -unsigned
```
```sql
LOAD '/path/to/three_d.duckdb_extension';
```

## Test

The project follows strict TDD (see [../AGENTS.md](../AGENTS.md)). SQL tests live in
`test/sql/`, C++ kernel tests in `test/cpp/`.

```sh
make test          # SQL tests, release build
make test_debug    # SQL tests, debug build
make test_cpp      # C++ kernel tests (Catch2, standalone)
make test_all      # test_debug + test_cpp
```

`make test_debug` is SQL-only — it does **not** run the C++ kernel tests. Use `test_cpp` or
`test_all` after touching `src/kernel/`.

Run a single test file:

```sh
./build/release/test/unittest "test/sql/geom_3d_measurements.test"
```

The `cityjson` interop tests are gated on the community `cityjson` extension and skip when it
is not registered with the runner — see [CITYJSON_INTEROP.md](./CITYJSON_INTEROP.md) for how
to run them (including the remote-data test against 3DBAG Delft).

## Managing dependencies (vcpkg)

DuckDB extensions can use [vcpkg](https://vcpkg.io) for dependency management. If/when a
dependency is added to `vcpkg.json`, set the toolchain before building:

```sh
export VCPKG_TOOLCHAIN_PATH=/path/to/vcpkg/scripts/buildsystems/vcpkg.cmake
```

The current kernel is self-contained and does not require an external geometry backend.

## Keeping DuckDB current

Update the pinned DuckDB submodule at least once per DuckDB LTS release to avoid CI drift:

```sh
cd duckdb && git fetch --all && git checkout <tag-or-commit> && cd ..
git add duckdb && git commit -m "Bump DuckDB submodule to <tag>"
```

Extension binaries only work for the DuckDB version they were built against; the CI in
`.github/workflows/` builds per target. See [UPDATING.md](./UPDATING.md) for the DuckDB API
update checklist.

## Distribution

Two common paths:

- **Community extensions** — the recommended route: submit a descriptor to the
  [community-extensions](https://github.com/duckdb/community-extensions) repository; users
  then `INSTALL three_d FROM community; LOAD three_d;`.
- **Custom repository / direct binary** — host the built `.duckdb_extension` and either point
  `custom_extension_repository` at it or `LOAD` the file directly (requires
  `allow_unsigned_extensions`).

## IDE setup (CLion)

Open `./duckdb/CMakeLists.txt` (not the repo-root `CMakeLists.txt`) as the project, then set
the project root back to this repo via *Tools → CMake → Change Project Root*. Add the
extension to the CMake options:

```
-DDUCKDB_EXTENSION_CONFIGS=<abs-path>/extension_config.cmake
```

Configure a `unittest` run/debug target for testing; pass
`--test-dir ../../.. "test/sql/*"` to scope it to this extension's tests.
