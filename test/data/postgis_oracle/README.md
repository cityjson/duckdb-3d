# PostGIS/SFCGAL differential oracle — golden data

These three CSVs hold **frozen reference values** computed by PostGIS + SFCGAL
for the duckdb-3d differential test (`test/sql/postgis_oracle.test`, design doc
§8.4). They are the *only* place PostGIS is involved: it runs offline, dev-time
only. `make test` reads the frozen values and never needs PostGIS, a container,
or the network.

| file | one row per | covers |
|---|---|---|
| `golden.csv` | geometry | measurement, accessors, bounding box, serialization |
| `golden_pairs.csv` | geometry pair | 3D distance and relation predicates |
| `golden_transforms.csv` | (geometry, transform op) | affine + CRS transforms |

## `golden.csv` — per-geometry reference

`geom_role` splits the input set by which side of the API consumes the bytes:

- **`solid`** — imported with `ST_3DFromWKB` into `SOLID_3D`: the two tetra
  fixtures, the hollow cube, the two-member cube collection (adjacent, and a
  second copy with the parts 1e6 apart), and the nine 3DBAG
  solids.
- **`geom`** — imported with `ST_Geom3DFromWKB` into `GEOM_3D`: one fixture per
  class the accessor surface dispatches on (point, line, multi-line, polygon,
  warped polygon, multi-point, multi-polygon). These are what make the accessor
  and serialization columns non-trivial; a `SOLID_3D` cannot hold them.

| column | meaning |
|---|---|
| `feature_id` | `fixture:*`, or a 3DBAG `NL.IMBAG.Pand.*` id |
| `lod` | LoD the solid was read at (`2.2`; empty for fixtures) |
| `geom_role` | `solid` or `geom` (see above) |
| `wkb_hex` | ISO WKB exported by `ST_3DAsWKB` / `ST_3DAsBinary` — the frozen bytes both engines see |
| `pg_geomtype` | `ST_GeometryType` |
| `pg_is_closed` | `ST_IsClosed` |
| `pg_area3d`, `pg_area_status` | `ST_3DArea`; `ok` or `rejected` |
| `pg_volume`, `pg_volume_status` | `abs(ST_Volume(ST_MakeSolid(·)))`; `ok` or `rejected` |
| `pg_hull_area` | `ST_Area(ST_ConvexHull(ST_Points(·)))` — 2D-XY convex-hull area |
| `pg_ndims`, `pg_coorddim`, `pg_dimension` | `ST_NDims` / `ST_CoordDim` / `ST_Dimension` |
| `pg_numgeom` | `ST_NumGeometries` |
| `pg_length3d` | `ST_3DLength` |
| `pg_proj_area` | `ST_Area(ST_Force2D(·))` — summed \|XY projection\| of every patch |
| `pg_min_x…pg_max_z` | the 3D bounding box (`ST_XMin` … `ST_ZMax`) |
| `pg_is_planar`, `pg_is_planar_status` | SFCGAL `ST_IsPlanar`; polygons only, else `rejected` |
| `pg_x`, `pg_y`, `pg_z`, `pg_xyz_status` | `ST_X`/`ST_Y`/`ST_Z`; points only, else `rejected` |
| `pg_wkb_roundtrip` | did `ST_AsBinary` re-emit `wkb_hex` unchanged? |
| `pg_wkt` | `ST_AsText` |
| `pg_geojson`, `pg_geojson_status` | `ST_AsGeoJSON`; `rejected` for `PolyhedralSurface` |
| `source`, `pg_version`, `sfcgal_version`, `geos_version` | provenance (constant per file) |

`rejected` = the PostGIS call raised. Three separate reasons produce it, and the
test filters on the matching status column rather than assuming one:

- **non-planar faces** — SFCGAL requires exactly-coplanar faces and rejects most
  real 3DBAG roofs (and the warped-polygon fixture, deliberately). The extension
  measures those by triangulation and is cross-checked against 3DBAG's own
  attributes in `cityjson_delft_remote.test` instead.
- **disconnected shells** — SFCGAL rejects the hollow cube outright ("not
  connected"). Interior-shell subtraction is checked by
  `test/cpp/test_inner_shell.cpp` and `st_3d_hollow_solid.test`.
- **class restriction** — `ST_X/Y/Z` on a non-point, `ST_IsPlanar` on a
  non-polygon, `ST_AsGeoJSON` on a `PolyhedralSurface`.

`pg_hull_area` feeds `ST_Points` to GEOS's `ST_ConvexHull` because GEOS rejects a
`PolyhedralSurface` directly; this hulls the vertex set, matching the extension's
`ST_3DFootprintArea(ST_3DConvexHull(g))`. GEOS has no planarity requirement, so
it is always present (no status column).

`pg_proj_area` is compared against `ST_3DFootprintArea` with a factor of two on
the two-sided classes: a vertical column crosses a closed shell twice, so
PostGIS's patch-wise sum is double the footprint, while a `(Multi)Polygon` is
single-sided and the two agree directly.

## `golden_pairs.csv` — distance & relation reference

One row per geometry **pair**. Unlike volume/area, SFCGAL's distance predicates
do not require planar faces, so these cover the real non-planar surfaces too.
The two geometries' WKB is not duplicated here — it is resolved from
`golden.csv` by joining on `feature_id`.

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
relation checks non-vacuous. Three cross-class pairs over the `geom` fixtures
reach the point/line/polygon branches the solid-only pairs never touch.

## `golden_transforms.csv` — affine & CRS transform reference

One row per (geometry, op), for the five affine transforms and the reprojection.
Parameters are fixed in `scripts/oracle/postgis_oracle_transforms.sql`:
`ST_Translate(g, 100.5, -50.25, 5.125)`, `ST_Scale(g, (2, 3, 0.5))`,
`ST_RotateX/Y/Z(g, 0.7)`, and `ST_Transform(ST_SetSRID(g, 28992), 4326)`.

| column | meaning |
|---|---|
| `feature_id`, `geom_role` | joined back to `golden.csv` for the input bytes |
| `op` | `translate`, `scale`, `rotatex`, `rotatey`, `rotatez`, `transform_28992_4326` |
| `pg_min_x…pg_max_z` | the 3D bounding box **after** the op |
| `pg_area3d`, `pg_area_status`, `pg_volume`, `pg_volume_status` | re-measured after the op |
| `pg_length3d`, `pg_proj_area` | re-measured after the op; need no planarity, so they carry the `geom` rows |
| `source`, `pg_version`, `sfcgal_version`, `geos_version` | provenance |

Re-measurement is the point: a rigid motion must not change volume or area, and
the anisotropic scale must multiply volume by exactly `det = 2·3·0.5 = 3`. This
is the oracle-backed regression guard for the bug fixed in `b019978`, where
origin-referenced volume tetrahedra made `ST_3DVolume` rotation-dependent on
real-world coordinates.

The reprojection op is emitted for the 3DBAG rows only — the fixtures sit at the
origin, far outside EPSG:28992's area of use, where PROJ's fallback pipeline
choice is not stable across versions. Its bounding box is compared with a 1e-7
absolute tolerance (the two PROJ builds differ; measured divergence is under
1e-9 degrees); its *area and volume*, being in degrees, are not compared at all.

## Provenance

- **source dataset:** `test/data/3dbag.city.jsonl` — a 3DBAG CityJSONSeq subset
  (9 Building/BuildingPart solids at LoD 2.2), committed alongside this file.
- **oracle image (pinned):** `postgis/postgis:16-3.4` → PostgreSQL 16.4,
  PostGIS 3.4.3, SFCGAL 1.3.8, GEOS 3.9.0 (backs `ST_ConvexHull`), PROJ 7.2.1.
  The first three are recorded per row and checked on regen.
- **WKB producer:** the `three_d` release extension via `ST_3DAsWKB` (solids)
  and `ST_3DAsBinary` (the `GEOM_3D` fixtures).

## Regenerating

Requires a container runtime. The defaults target Apple `container`; Docker and
rootless Podman both work by overriding two variables. From the repo root:

```sh
# Apple `container` (default)
just oracle-up && just oracle-regen && just oracle-down

# Podman (rootless) or Docker
export ORACLE_RUNTIME=podman ORACLE_ARCH_FLAG="--platform linux/amd64"
just oracle-up
just oracle-regen
just oracle-down
```

`oracle-regen` reads `wkb_hex` straight from `golden.csv`, so it works on any
DuckDB/toolchain version and needs no cityjson. A clean run leaves all three
CSVs **byte-identical** (deterministic sort + `%.17g` floats).

Only when the *fixture* (`3dbag.city.jsonl`) or the input set changes do you need
to re-derive the WKB from CityJSON:

```sh
just oracle-reexport   # DuckDB CLI + cityjson re-export, then recompute
```

That path needs a **locally built** `duckdb-cityjson` — the published community
extension still emits the pre-per-LoD column shape this repo no longer targets.
Build the sibling checkout (`cd ../duckdb-cityjson && GEN=ninja make release`),
or point `CITYJSON_EXTENSION` at another build. See `docs/CITYJSON_INTEROP.md`.
