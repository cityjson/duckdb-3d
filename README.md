# duckdb-3d

> ⚠️ **Experimental.** This library is under active development and should be considered experimental. Its API, output schema, and on-disk formats may change without notice, and bugs are expected — including ones that can affect data correctness. Do not rely on it for production workloads yet, and verify results against a trusted source before use. Please report issues you encounter.

A [DuckDB](https://duckdb.org) extension for **3D solid processing**. It makes the polyhedral
solids in 3D city models — buildings from CityJSON / [3DBAG](https://3dbag.nl), CityParquet,
and similar sources — first-class, queryable values in SQL: measure enclosed volume, check
whether a solid is closed and manifold, compute footprint area and building height, and run
3D distance queries, all inside DuckDB.

DuckDB's built-in geometry surface is 2D / simple-features centric. `duckdb-3d` adds the
solid-aware types and functions that 3D workflows need, without pulling in a heavyweight
geometry backend.

## Highlights

- **`SOLID_3D`** — a dedicated type for closed polyhedral solids, backed by a compact,
  versioned binary payload that preserves shell/face topology (triangulation is a derived
  cache, never the source of truth).
- **`GEOM_3D`** — a general 3D geometry type (points, lines, polygons, multis, polyhedral
  surfaces) for the class-generic accessor, distance, and serialization functions.
- **Validation, not repair** — `duckdb-3d` reports closedness, manifoldness, orientation, and
  degeneracy; it never silently "fixes" geometry.
- **Coexists with `spatial`** — 3D operations use `ST_3D*` names (PostGIS's convention for 3D
  variants), so `three_d` and DuckDB's `spatial` extension load together in one session, in
  any order. A curated 3D subset, not full PostGIS parity.
- **Self-contained kernel** — no CGAL/SFCGAL dependency.

## Quick start

```sql
INSTALL cityjson FROM community;   -- one-time; reads CityJSON into WKB
LOAD cityjson;
LOAD three_d;

-- Measure real buildings straight from a remote CityJSONSeq server
SELECT id,
       ROUND(ST_3DVolume(solid), 1)                  AS volume_m3,
       ROUND(ST_3DFootprintArea(solid), 1)           AS footprint_m2,
       ROUND(ST_3DZMax(solid) - ST_3DZMin(solid), 2) AS height_m
FROM (
  SELECT id, ST_3DTryFromWKB(geometry, geometry_properties) AS solid
  FROM read_cityjsonseq(
    'https://cityjson.open3d.city/cityjsonseq/delft.city.jsonl', lod => '2.2')
  WHERE geometry IS NOT NULL
)
WHERE ST_3DValidationReport(solid).is_valid
LIMIT 5;
```

```
┌──────────────────────────────────┬───────────┬──────────────┬──────────┐
│                id                │ volume_m3 │ footprint_m2 │ height_m │
├──────────────────────────────────┼───────────┼──────────────┼──────────┤
│ NL.IMBAG.Pand.0503100000012869-0 │ 19.5      │ 7.2          │ 2.75     │
│ NL.IMBAG.Pand.0503100000016459-0 │ 27.8      │ 10.3         │ 2.73     │
│ NL.IMBAG.Pand.0503100000005156-0 │ 637.0     │ 99.2         │ 10.43    │
│ NL.IMBAG.Pand.0503100000019786-0 │ 151.3     │ 48.6         │ 3.13     │
│ NL.IMBAG.Pand.0503100000018426-0 │ 881.3     │ 87.3         │ 10.15    │
└──────────────────────────────────┴───────────┴──────────────┴──────────┘
```

Geometry can equally come from a CityParquet file — the `geometry_properties_lod*` `STRUCT`
is accepted directly, with no `to_json(...)` round-trip:

```sql
SELECT ST_3DVolume(ST_3DFromWKB(geometry_lod2_2, geometry_properties_lod2_2))
FROM read_parquet('building.parquet')
WHERE geometry_lod2_2 IS NOT NULL;
```

**Sample data.** The examples throughout the docs use the 3DBAG Delft tile:
[`delft.city.jsonl`](https://cityjson.open3d.city/cityjsonseq/delft.city.jsonl) (CityJSONSeq)
and [`delft.city.json`](https://cityjson.open3d.city/cityjson/delft.city.json) (CityJSON).

## Representative functions

A digest — the **[full reference with runnable examples is in docs/FUNCTIONS.md](docs/FUNCTIONS.md)**
(55 functions).

| Function | Does |
| --- | --- |
| `ST_3DFromWKB(wkb [, props])` | Build a `SOLID_3D` from WKB, optionally using a shell-grouping sidecar. `ST_3DTryFromWKB` returns `NULL` instead of raising. |
| `ST_Geom3DFromWKB(wkb)` | Build a `GEOM_3D` — the entry point for distance and serialization. |
| `ST_3DValidationReport(solid)` | Full validity struct: closed, manifold, oriented, plus per-check counters and a diagnostic message. |
| `ST_3DVolume(solid)` | Enclosed volume. Interior shells (cavities) subtract automatically. Raises on invalid solids. |
| `ST_3DFootprintArea(solid)` | 2D ground area of the XY projection. |
| `ST_3DArea(solid)` | Total 3D surface area of all faces. |
| `ST_3DBounds(solid)` | Cached 3D bounding box as a struct — height is `ST_3DZMax − ST_3DZMin`. |
| `ST_3DDistance(a, b)` | Minimum 3D distance. `ST_3DDWithin(a, b, d)` is the cheap bbox-pruned predicate. |
| `ST_3DTransform(geom, src, tgt)` | CRS reprojection via PROJ — X/Y reprojected, **Z preserved**. |
| `ST_3DExtrude(polygon, height)` | Extrude a footprint into a closed LoD1 prism. |
| `ST_3DAsText` / `ST_3DAsGeoJSON` / `ST_3DAsWKB` | Serialize to WKT, GeoJSON, or WKB. |

Also available: shell/face counts, `ST_3DIsClosed` / `ST_3DIsManifold` / `ST_3DIsOriented`,
`ST_3DCentroid`, `ST_3DConvexHull`, `ST_MakeSolid`, `ST_3DTranslate` / `ST_3DScale` /
`ST_3DRotateX/Y/Z`, `ST_3DClosestPoint`, `ST_3DShortestLine`, and more — see
[docs/FUNCTIONS.md](docs/FUNCTIONS.md).

## Verified against real data

`duckdb-3d`'s measurements are cross-checked against 3DBAG's own independently computed
attributes for the Delft tile: **median volume error 0.017 %** across ~1100 buildings, with
footprint areas matching exactly. The math is additionally cross-checked offline against
PostGIS + SFCGAL as a differential oracle (never a build or runtime dependency).

## Using with DuckDB `spatial`

Load both in one session, in any order. `spatial` handles the generic 2D `GEOMETRY`
vocabulary; `three_d` handles the 3D solids:

```sql
LOAD spatial;
LOAD three_d;

SELECT ST_Area(footprint)                                 AS ground_area_m2, -- spatial, 2D
       ST_3DVolume(ST_3DFromWKB(solid_wkb, solid_props))  AS volume_m3       -- three_d, 3D
FROM buildings;
```

## Building

Targets DuckDB `v1.5.x`. Clone with submodules, then build:

```sh
git clone --recurse-submodules <repo-url>
cd duckdb-3d
make                 # first build compiles DuckDB too; subsequent builds are incremental
GEN=ninja make       # much faster, with ninja + ccache installed
```

Artifacts:

- `build/release/duckdb` — a DuckDB shell with `three_d` preloaded
- `build/release/extension/three_d/three_d.duckdb_extension` — the loadable extension

```sh
make test_full     # configure + build + every test, no skips
make test          # SQL tests against the release build
make test_debug    # SQL tests against the debug build
make test_cpp      # C++ kernel tests
make test_all      # test_debug + test_cpp
```

`make test_full` is the self-contained one: it builds, stages the `cityjson` / `spatial`
extensions the gated tests need, and runs both suites. The others run against whatever build
already exists.

Full build, test, and distribution notes: [docs/README.md](docs/README.md).

## Documentation

| Document | Contents |
| --- | --- |
| [docs/FUNCTIONS.md](docs/FUNCTIONS.md) | **Function reference** — every function, with signatures and runnable examples |
| [docs/EXAMPLE.md](docs/EXAMPLE.md) | Hands-on walkthrough against real 3DBAG data |
| [docs/DESIGN_DOC.md](docs/DESIGN_DOC.md) | Architecture & design philosophy: type model, layering, invariants |
| [docs/CITYJSON_INTEROP.md](docs/CITYJSON_INTEROP.md) | Composing with the `cityjson` extension; running the interop tests |
| [docs/FUTURE_WORK.md](docs/FUTURE_WORK.md) | Deferred design decisions |
| [docs/README.md](docs/README.md) | Build & development notes |
| [docs/UPDATING.md](docs/UPDATING.md) | Keeping the DuckDB submodule current |
| [AGENTS.md](AGENTS.md) | Contributor / coding-agent guide (TDD workflow, layering) |

## Project context

`duckdb-3d` is part of the **CityParquet + CityLake** research workspace — a cloud-native
delivery stack for 3D city models developed in the 3D Geoinformation group at TU Delft. It is
the bridge between CityParquet's stored WKB geometry and 3D processing: validation,
measurement, and analysis of the solids a city model carries.

## License

MIT — see [LICENSE](LICENSE).
