# PostGIS/SFCGAL differential oracle — golden data

`golden.csv` holds **frozen reference values** computed by PostGIS + SFCGAL for
the duckdb-3d differential test (`test/sql/postgis_oracle.test`, design doc
§9.5.1). It is the *only* place PostGIS is involved: it runs offline, dev-time
only. `make test` reads the frozen values and never needs PostGIS, a container,
or the network.

## What each row is

One row per geometry fed to the oracle. Columns:

| column | meaning |
|---|---|
| `feature_id` | `fixture:tetra` / `fixture:open_tetra`, or a 3DBAG `NL.IMBAG.Pand.*` id |
| `lod` | LoD the solid was read at (`2.2`; empty for fixtures) |
| `geom_role` | `solid` |
| `wkb_hex` | ISO WKB (`PolyhedralSurface Z`) exported by `ST_3DAsWKB` — the frozen bytes both engines see |
| `pg_geomtype` | `ST_GeometryType` in PostGIS (sanity: `ST_PolyhedralSurface`) |
| `pg_is_closed` | `ST_IsClosed` (boolean) |
| `pg_area3d`, `pg_area_status` | `ST_3DArea`; `ok` or `rejected` |
| `pg_volume`, `pg_volume_status` | `abs(ST_Volume(ST_MakeSolid(·)))`; `ok` or `rejected` |
| `pg_hull_area` | `ST_Area(ST_ConvexHull(ST_Points(·)))` — 2D-XY convex-hull area |
| `source`, `pg_version`, `sfcgal_version`, `geos_version` | provenance (constant per file) |

`pg_hull_area` feeds `ST_Points` to GEOS's `ST_ConvexHull` because GEOS rejects a
`PolyhedralSurface` directly; this hulls the vertex set, matching the extension's
`ST_Area(ST_ConvexHull(g))`. GEOS has no planarity requirement, so it is always
`ok` (no status column).

`rejected` = SFCGAL raised. SFCGAL requires exactly-coplanar faces and rejects
most real 3DBAG roofs; the extension measures those by triangulation and is
cross-checked against 3DBAG attributes in `cityjson_delft_remote.test` instead.

### `golden_pairs.csv` — distance & relation reference

One row per geometry **pair**, for the 3D distance/relation functions. Unlike
volume/area, SFCGAL's distance predicates do not require planar faces, so these
cover the real non-planar surfaces too. The two geometries' WKB is not
duplicated here — it is resolved from `golden.csv` by joining on `feature_id`.

| column | meaning |
|---|---|
| `feature_a`, `feature_b` | the paired feature ids (found in `golden.csv`) |
| `threshold` | distance threshold for the `*within` predicates |
| `pg_dist3d`, `pg_maxdist3d` | `ST_3DDistance` / `ST_3DMaxDistance` |
| `pg_intersects` | `ST_3DIntersects` |
| `pg_dwithin`, `pg_dfullywithin` | `ST_3DDWithin` / `ST_3DDFullyWithin` at `threshold` |
| `pg_shortline_len` | `ST_3DLength(ST_3DShortestLine(a, b))` |
| `pg_closestpoint_dist` | `ST_3DDistance(ST_3DClosestPoint(a, b), b)` |
| `source`, `pg_version`, `sfcgal_version`, `geos_version` | provenance |

The last two are scalars extracted from geometry-returning functions: we never
round-trip PostGIS geometry back into the extension (PostGIS emits EWKB, whose
SRID flag the extension's ISO-WKB parser rejects). Both equal the 3D distance by
construction, so they also serve as an internal consistency check.

Pairs are chosen so every boolean predicate straddles true and false (two
distinct buildings ≈ 517.6 m apart, plus self-pairs), keeping the Phase B
relation checks non-vacuous.

## Provenance

- **source dataset:** `test/data/3dbag.city.jsonl` — a 3DBAG CityJSONSeq subset
  (9 Building/BuildingPart solids at LoD 2.2), committed alongside this file.
- **oracle image (pinned):** `postgis/postgis:16-3.4` → PostgreSQL 16.4,
  PostGIS 3.4.3, SFCGAL 1.3.8, GEOS 3.9.0 (backs `ST_ConvexHull`). All three
  library versions are recorded per row and checked on regen.
- **WKB producer:** the `three_d` release extension via `ST_3DAsWKB`.

## Regenerating

Requires a container runtime; defaults target Apple `container` (Docker works
too — see the note in `justfile`). From the repo root:

```sh
just oracle-up       # start postgis/postgis:16-3.4, wait for readiness
just oracle-regen    # recompute pg_* from the FROZEN wkb_hex (no DuckDB/cityjson)
just oracle-down     # stop + remove the container
```

`oracle-regen` reads `wkb_hex` straight from this file, so it works on any
DuckDB/toolchain version. A clean run leaves `golden.csv` **byte-identical**
(deterministic sort + `%.17g` floats).

Only when the *fixture* (`3dbag.city.jsonl`) or the input set changes do you need
to re-derive the WKB from CityJSON:

```sh
just oracle-reexport   # DuckDB CLI + cityjson re-export, then recompute
```

This needs a DuckDB version for which the `cityjson` community extension is
published (≤ v1.5.3 at time of writing); the default `oracle-regen` path avoids
that coupling.
