# CityJSON Interop

> For a task-oriented walkthrough (measuring buildings, validating solids, and
> cross-checking against 3DBAG ground truth on the remote Delft dataset), see
> [EXAMPLE.md](./EXAMPLE.md); for breadth — every public function run against
> real data — see [TESTING.md](./TESTING.md). This file covers the composition
> mechanics and how to run the interop tests.

The design doc §7 specifies that `duckdb-3d` integrates with the
[`duckdb-cityjson`](https://github.com/cityjson/duckdb-cityjson) extension via
plain SQL composition: `cityjson` produces WKB plus a `geometry_properties`
sidecar, and `duckdb-3d` consumes both through the 2-arg `ST_3DFromWKB` overload.
No CityJSON parsing happens in this repo, and nothing in the kernel is
conditional on which `cityjson` build produced the bytes.

## Which `cityjson` build

**The sibling `../duckdb-cityjson` checkout, built locally.** Not
`INSTALL cityjson FROM community`.

The published community extension is an older build that still emits a flat
`geometry BLOB` + `geometry_properties VARCHAR` pair. The current one emits **one
column pair per LoD present**:

| | community (stale) | `../duckdb-cityjson` (targeted here) |
| --- | --- | --- |
| Geometry | `geometry` `BLOB` | `geometry_lod2_2` `BLOB` — one per LoD |
| Sidecar | `geometry_properties` `VARCHAR` (JSON) | `geometry_properties_lod2_2` `STRUCT` |

`ST_3DFromWKB` accepts **both** sidecar forms — the `VARCHAR` and `STRUCT`
overloads are [documented](./FUNCTIONS.md#st_3dfromwkb--st_3dtryfromwkb) and both
are covered by `test/sql/st_3d_from_wkb_struct.test` — so this is not a kernel
concern. What changed is the **column names**, so SQL written for one build does
not bind against the other. Every example and gated test in this repo targets the
current shape only; support for the flat shape was dropped deliberately rather
than carried on a compatibility branch.

Two more naming rules:

- The suffix is the **normalised** LoD: `lod => '2'` produces `geometry_lod2_0`,
  not `geometry_lod2`. `lod => '2.2'` produces `geometry_lod2_2`.
- An object can carry several LoDs at once, so `DESCRIBE` the reader rather than
  assuming a single geometry column:

```sql
SELECT column_name, column_type
FROM (DESCRIBE SELECT * FROM read_cityjson('test/data/unit_cube.city.json', lod => '2.2'))
WHERE column_name LIKE 'geometry%';
```
```
┌────────────────────────────┬────────────────────────────────────────────────────────────────────────────────────────┐
│        column_name         │                                      column_type                                       │
├────────────────────────────┼────────────────────────────────────────────────────────────────────────────────────────┤
│ geometry_lod2_2            │ BLOB                                                                                   │
│ geometry_properties_lod2_2 │ STRUCT("type" VARCHAR, surfaces VARCHAR, face_semantics INTEGER[], shells INTEGER[][]) │
└────────────────────────────┴────────────────────────────────────────────────────────────────────────────────────────┘
```

## Setup

Both repos pin the same DuckDB core (`v1.5.4`), so the two locally built
extensions load into one process without an ABI complaint:

```sh
GEN=ninja make                                   # ./build/release/duckdb, three_d linked in
(cd ../duckdb-cityjson && GEN=ninja make release)
```

`cityjson` is loaded **by path**:

```sh
./build/release/duckdb -unsigned
```
```sql
LOAD '../duckdb-cityjson/build/release/extension/cityjson/cityjson.duckdb_extension';
LOAD three_d;
```

## Smoke test

`test/sql/cityjson_interop.test` covers the full pipeline against a tiny
unit-cube fixture (`test/data/unit_cube.city.json`). By hand:

```sh
./build/release/duckdb -unsigned -c "
LOAD '../duckdb-cityjson/build/release/extension/cityjson/cityjson.duckdb_extension';
LOAD three_d;

SELECT id,
       ST_3DNumFaces(solid)              AS faces,
       ST_3DIsClosed(solid)              AS closed,
       ROUND(ST_3DSurfaceArea(solid), 6) AS area,
       ROUND(ST_3DVolume(solid), 6)      AS volume
FROM (
  SELECT id, ST_3DFromWKB(geometry_lod2_2, geometry_properties_lod2_2) AS solid
  FROM read_cityjson('test/data/unit_cube.city.json', lod => '2.2')
  WHERE geometry_lod2_2 IS NOT NULL
);
"
```

```
┌──────┬───────┬────────┬──────┬────────┐
│  id  │ faces │ closed │ area │ volume │
├──────┼───────┼────────┼──────┼────────┤
│ cube │ 6     │ true   │ 6.0  │ 1.0    │
└──────┴───────┴────────┴──────┴────────┘
```

## Hollow solids (interior shells) — the `shells` contract end-to-end

A CityJSON `Solid` is encoded as one WKB `PolyhedralSurface Z` whose faces are
**all shells flattened into one list** — the exterior/interior distinction is
gone from the WKB. `duckdb-cityjson` preserves it in the `geometry_properties`
`shells` key (per-shell face counts), and `duckdb-3d` uses that to rebuild the
shell partition, so a cavity's volume subtracts instead of being ignored.

Using `test/data/hollow_solid.city.json` — an outer cube (V=64) with a
concentric cavity (V=8):

```sh
./build/release/duckdb -unsigned -c "
LOAD '../duckdb-cityjson/build/release/extension/cityjson/cityjson.duckdb_extension';
LOAD three_d;

SELECT geometry_properties_lod2_0 AS props
FROM read_cityjson('test/data/hollow_solid.city.json', lod => '2')
WHERE geometry_lod2_0 IS NOT NULL;

SELECT ST_3DNumShells(s)        AS shells,
       ST_3DIsClosed(s)         AS closed,
       ROUND(ST_3DVolume(s), 6) AS volume
FROM (
  SELECT ST_3DFromWKB(geometry_lod2_0, geometry_properties_lod2_0) AS s
  FROM read_cityjson('test/data/hollow_solid.city.json', lod => '2')
  WHERE geometry_lod2_0 IS NOT NULL
);
"
```

Expected — the cavity is recovered from `shells` and subtracted (64 − 8 = 56):

```
┌───────────────────────────────────────────────────────────────────────────────┐
│                                     props                                     │
├───────────────────────────────────────────────────────────────────────────────┤
│ {'type': Solid, 'surfaces': NULL, 'face_semantics': NULL, 'shells': [[6, 6]]} │
└───────────────────────────────────────────────────────────────────────────────┘
┌────────┬────────┬────────┐
│ shells │ closed │ volume │
├────────┼────────┼────────┤
│ 2      │ true   │ 56.0   │
└────────┴────────┴────────┘
```

Note the nesting: the STRUCT sidecar types `shells` as `List<List<Int32>>`
unconditionally, so a plain `Solid` arrives as `[[6, 6]]` rather than the flat
`[6, 6]` a JSON sidecar would carry. `ST_3DFromWKB` accepts both — pinned by
`test/sql/st_3d_hollow_solid.test` and `test/cpp/test_metadata.cpp` — so reading
a CityParquet `geometry_properties_lod*` STRUCT, or the same STRUCT rendered to
text with `to_json(...)`, needs no translation.

The winding matters: `duckdb-3d` enforces that an interior shell is wound
opposite the exterior (CityGML §9.3). A cavity wound the *same* way as the
exterior would silently *add* its volume, so it is rejected instead —
`ST_3DVolume` raises `solid has inconsistent orientation`, and the imported
solid reports `ST_3DIsOriented(s) = false` /
`ST_3DValidationReport(s).is_valid = false`. Correct volumes therefore never
depend on trusting the producer's winding blindly.

## Remote CityJSONSeq

For remote CityJSONSeq data, pass `lod => '...'` so `cityjson` emits WKB
columns, and use `ST_3DTryFromWKB` while exploring real-world data:

```sh
ASAN_OPTIONS=detect_container_overflow=0 ./build/debug/duckdb -unsigned -c "
LOAD '../duckdb-cityjson/build/release/extension/cityjson/cityjson.duckdb_extension';
LOAD three_d;

WITH solids AS (
  SELECT id, ST_3DTryFromWKB(geometry_lod2_2, geometry_properties_lod2_2) AS solid
  FROM read_cityjsonseq(
    'https://cityjson.open3d.city/cityjsonseq/delft.city.jsonl',
    lod => '2.2'
  )
  WHERE geometry_lod2_2 IS NOT NULL
)
SELECT id,
       ST_3DNumFaces(solid) AS faces,
       ST_3DIsClosed(solid) AS closed,
       ROUND(ST_3DSurfaceArea(solid), 3) AS area
FROM solids
WHERE solid IS NOT NULL
LIMIT 10;
"
```

## Running the gated tests under sqllogic

```sh
make test_full
```

Five test files are gated on `require cityjson`
(`cityjson_interop`, `cityjson_hollow_solid`, `cityjson_multisolid`,
`cityjson_3dbag_attributes`, `cityjson_delft_remote`), and one on
`require spatial` (`spatial_coexist`).
Under `make test` / `make test_debug` they skip; `make test_full` does the setup
that makes them execute, and treats a skip as a failure. It needs network access
— the httpfs/spatial download on the first run, and the remote Delft fixture on
every run. A green run reports **33 test cases, 0 skipped**.

Note `test_full` runs against the **release** build, unlike `make test_debug`.
The gated tests load third-party extensions that exist only as release-built
binaries, and a debug DuckDB tracks allocations the release allocator does not:
streaming the remote Delft tile through a release-built `cityjson` inside the
debug binary aborts with `Assertion triggered in allocator.cpp:
allocation_count >= size`. Mixing the two flavours is not a supported
configuration. Debug-allocator coverage of everything else comes from
`make test_all`.

Three things must hold simultaneously when the runner starts, and `test_full`
arranges all three:

1. `DUCKDB_TEST_AUTOLOADING=available` — flips the runner from the default
   "no autoload" mode. Without it every `require cityjson` skips.
2. **`cityjson`, `spatial` *and* `three_d` must all be files in the runner's
   local repository** (`${LOCAL_EXTENSION_REPO}`, which `test_full` points at
   `build/release/repository`; the runner's own default is
   `<build dir>/repository`). In autoloading mode the runner answers
   `require <ext>` with `INSTALL <ext> FROM '<repo>'` followed by `LOAD <ext>` —
   *including for `three_d`, which is statically linked*. The `LOAD` would
   succeed on its own, but the `INSTALL` runs first and fails if no file is
   there, and the test then skips on `require three_d`. `test_full` stages the
   freshly built `three_d.duckdb_extension` alongside the other two. (This is
   why the gated tests skipped even before the community build went stale.)
3. `httpfs` must already be installed in the runner's DuckDB home. The remote
   Delft test autoloads it, and autoinstall resolves against the official
   repository rather than the local one — so staging it in the local repository
   does *not* satisfy this. Without it the run fails at
   `cityjson_delft_remote.test` with `Extension Autoloading Error`.

The `cityjson` file comes from `CITYJSON_EXTENSION`, which defaults to
`../duckdb-cityjson/build/release/extension/cityjson/cityjson.duckdb_extension`.
`test_full` fails fast with build instructions if it is not there. Overriding it
is how you test against another build:

```sh
make test_full CITYJSON_EXTENSION=/path/to/cityjson.duckdb_extension
```

Nothing is downloaded from the community repository any more, so the old
"origin is different" conflict — DuckDB refusing to re-`INSTALL` an extension
whose origin does not match the installed copy — no longer arises for
`cityjson`. `build/test_home` is still wiped and reseeded with `httpfs` alone
before each run — it has to be, since an extension installed there from one
repository path cannot be re-installed from another — and `build/ext_cache`
still holds the downloaded `httpfs` and `spatial` copies so the download happens
once.

## Caveats

- `read_cityjson(path, lod => '...')` is the per-LoD mode that emits the
  `geometry_lod<X>` BLOB / `geometry_properties_lod<X>` STRUCT pair. Without the
  `lod` parameter, cityjson emits `geom_lod*` STRUCTs instead and `ST_3DFromWKB`
  cannot consume them — pass `lod => '...'` always. The same applies to
  `read_cityjsonseq`.
- DuckDB debug builds may run with AddressSanitizer enabled. On macOS, loading a
  release-built `cityjson` into a debug shell can trip an ASan
  `container-overflow` check while opening remote files. If the stack trace
  points at DuckDB settings during `read_cityjsonseq('https://...')`, rerun the
  debug shell with `ASAN_OPTIONS=detect_container_overflow=0`.
- `duckdb-3d` reads the CityParquet **spec §8** `geometry_properties` form: a
  string `"type"` and a `"shells"` key giving per-shell face counts. Both the
  flat (`[12]`, `[6,6]`) and nested (`[[12]]`, `[[6],[4]]`) shapes are accepted,
  so a JSON sidecar and a CityParquet STRUCT sidecar both work. `"type"` is
  informational and a non-string value is tolerated.
- Filter `WHERE geometry_lod<X> IS NOT NULL` upstream — objects without the
  requested LoD have a NULL geometry and would propagate NULL through the
  constructor.
- CityParquet round trip: a `geometry_properties_lod*` column read back from
  Parquet is the same STRUCT, so `ST_3DFromWKB` consumes it unchanged. See
  [TESTING.md](./TESTING.md) Part C.
