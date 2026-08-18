# CityJSON Interop

> For a task-oriented walkthrough (measuring buildings, validating solids, and
> cross-checking against 3DBAG ground truth on the remote Delft dataset), see
> [EXAMPLE.md](./EXAMPLE.md). This file covers the composition mechanics and how
> to run the interop tests.

The design doc §7 specifies that `duckdb-3d` integrates with the
[`duckdb-cityjson`](https://github.com/cityjson/duckdb-cityjson) community
extension via plain SQL composition: `cityjson` produces WKB plus
`geometry_properties` JSON, and `duckdb-3d` consumes both through the 2-arg
`ST_3DFromWKB` overload.

## Smoke test

`test/sql/cityjson_interop.test` covers the full pipeline against a tiny
unit-cube fixture (`test/data/unit_cube.city.json`).

The test is gated on `require cityjson`. The DuckDB sqllogic-test runner
treats community extensions as "excluded from autoloading", so under
`make test_debug` the test gracefully skips unless extra setup is in
place.

## Running the smoke test manually

The simplest path — confirmed working — is to run the queries directly
through the debug shell, which uses your user-wide extension install:

```sh
make debug                                # one-time
./build/debug/duckdb -unsigned -c \
  "INSTALL cityjson FROM community;"     # one-time

./build/debug/duckdb -unsigned -c "
LOAD cityjson;
LOAD three_d;

SELECT id,
       ST_3DNumFaces(solid)        AS faces,
       ST_3DIsClosed(solid)        AS closed,
       ROUND(ST_3DSurfaceArea(solid), 6) AS area,
       ROUND(ST_3DVolume(solid), 6)      AS volume
FROM (
  SELECT id, ST_3DFromWKB(geometry, geometry_properties) AS solid
  FROM read_cityjson('test/data/unit_cube.city.json', lod => '2.2')
  WHERE geometry IS NOT NULL
);
"
```

Expected:

```
id   | faces | closed | area | volume
cube | 6     | true   | 6.0  | 1.0
```

## Hollow solids (interior shells) — the `shells` contract end-to-end

A CityJSON `Solid` is encoded as one WKB `PolyhedralSurface Z` whose faces are
**all shells flattened into one list** — the exterior/interior distinction is
gone from the WKB. `duckdb-cityjson` preserves it in the spec §8
`geometry_properties` `shells` key (per-shell face counts), and `duckdb-3d` uses
that to rebuild the shell partition, so a cavity's volume subtracts instead of
being ignored.

Using `test/data/hollow_solid.city.json` — an outer cube (V=64) with a
concentric cavity (V=8):

```sh
./build/release/duckdb -unsigned -c "
LOAD cityjson;
LOAD three_d;

SELECT geometry_properties FROM
  read_cityjson('test/data/hollow_solid.city.json', lod => '2')
  WHERE geometry IS NOT NULL;
-- {\"lod\":\"2\",\"shells\":[6,6],\"type\":\"Solid\"}

SELECT ST_3DNumShells(s)          AS shells,
       ST_3DIsClosed(s)           AS closed,
       ROUND(ST_3DVolume(s), 6)   AS volume
FROM (
  SELECT ST_3DFromWKB(geometry, geometry_properties) AS s
  FROM read_cityjson('test/data/hollow_solid.city.json', lod => '2')
  WHERE geometry IS NOT NULL
);
"
```

Expected — the cavity is recovered from `shells:[6,6]` and subtracted
(64 − 8 = 56):

```
shells | closed | volume
2      | true   | 56.0
```

The winding matters: `duckdb-3d` enforces that an interior shell is wound
opposite the exterior (CityGML §9.3). A cavity wound the *same* way as the
exterior would silently *add* its volume, so it is rejected instead —
`ST_3DVolume` raises `solid has inconsistent orientation` and `ST_3DTryFromWKB`
returns a solid that fails `ST_3DIsValid`. Correct volumes therefore never
depend on trusting the producer's winding blindly.

> Requires a `cityjson` build that emits the spec §8 `shells` key. An older
> build that omits it still returns 56 here (volume subtraction is
> shell-grouping invariant for a disjoint cavity), but as a single merged shell
> and without the winding check.

## Remote CityJSONSeq smoke test

For remote CityJSONSeq data, pass `lod => '...'` so `cityjson` emits WKB
columns, and use `ST_3DTryFromWKB` while exploring real-world data:

```sh
ASAN_OPTIONS=detect_container_overflow=0 ./build/debug/duckdb -unsigned -c "
LOAD cityjson;
LOAD three_d;

WITH solids AS (
  SELECT id, ST_3DTryFromWKB(geometry, geometry_properties) AS solid
  FROM read_cityjsonseq(
    'https://cityjson.open3d.city/cityjsonseq/delft.city.jsonl',
    lod => '2.2'
  )
  WHERE geometry IS NOT NULL
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

## Running the smoke test under sqllogic

To make `test/sql/cityjson_interop.test` run under `make test_debug`, two
things must hold simultaneously when the runner starts:

1. `DUCKDB_TEST_AUTOLOADING=available` (or `=all`) — flips the runner from
   the default "no autoload" mode.
2. The cityjson extension binary must be discoverable by the runner. The
   runner looks at `${LOCAL_EXTENSION_REPO}` (env override) or otherwise at
   `build/debug/repository`. Either copy
   `~/.duckdb/extensions/<duckdb-version>/<platform>/cityjson.duckdb_extension*`
   into `build/debug/repository/<duckdb-version>/<platform>/`, or set
   `LOCAL_EXTENSION_REPO` to a directory laid out the same way.

Note that DuckDB refuses to re-`INSTALL` an extension whose origin doesn't
match the previously installed copy. If you've already
`INSTALL cityjson FROM community`, the runner's `INSTALL ... FROM '<path>'`
step will fail with "origin is different" and the test will skip. The
practical workaround is to run with a clean DuckDB home dir, for example
by exporting `HOME=$(mktemp -d)` for the test invocation.

## Caveats

- `read_cityjson(path, lod => '...')` is the per-LOD mode that emits a
  `geometry BLOB` plus `geometry_properties VARCHAR` column. Without the
  `lod` parameter, cityjson emits a `STRUCT` instead and `ST_3DFromWKB`
  cannot consume it — pass `lod => '...'` always.
- The same applies to CityJSONSeq inputs. Use
  `read_cityjsonseq(url_or_path, lod => '2.2')` when you want WKB columns;
  plain `read_cityjsonseq(url_or_path)` returns LOD-specific `geom_lod*`
  structs instead.
- DuckDB debug builds may run with AddressSanitizer enabled. On macOS,
  loading the release-built community `cityjson` extension can trip an ASan
  `container-overflow` check while opening remote files. If the stack trace
  points at DuckDB settings during `read_cityjsonseq('https://...')`, rerun
  the debug shell with `ASAN_OPTIONS=detect_container_overflow=0`.
- `duckdb-3d` reads the CityParquet **spec §8** `geometry_properties` form:
  a string `"type"` and a `"shells"` key giving per-shell face counts (flat
  `[12]` for a `Solid`, nested `[[12],[8,4]]` per solid for a
  `MultiSolid`/`CompositeSolid`). `"shells"` is what recovers the inner-shell /
  multi-solid partition the flat WKB drops. `"type"` is informational and a
  non-string value from a pre-spec producer is tolerated, so plain-path interop
  with older `cityjson` builds still works (it just imports one shell per WKB
  member).
- A CityParquet `geometry_properties_lod*` column (cityparquet-rs's M4 STRUCT)
  nests `shells` as `List<List<Int32>>` unconditionally, so a plain `Solid`
  serializes with one extra wrapping level (`[[12]]`, `[[12,4]]`) rather than
  duckdb-cityjson's flat form. `ST_3DFromWKB`/`ST_3DTryFromWKB` accept both —
  confirmed in `test/sql/st_3d_hollow_solid.test` and
  `test/cpp/test_metadata.cpp` — so reading a `geometry_properties_lod*`
  STRUCT converted to text (e.g. via `to_json(...)`) needs no translation.
- Filter `WHERE geometry IS NOT NULL` upstream — objects without the
  requested LOD have a NULL geometry and would propagate NULL through
  the constructor.
