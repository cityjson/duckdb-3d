# duckdb-3d

A [DuckDB](https://duckdb.org) extension for **3D solid processing**. It makes the polyhedral
solids in 3D city models — buildings from CityJSON / [3DBAG](https://3dbag.nl), CityParquet,
and similar sources — first-class, queryable values in SQL: measure enclosed volume, check
whether a solid is closed and manifold, compute footprint area and building height, and run
3D distance queries, all inside DuckDB.

DuckDB's built-in geometry surface is 2D / simple-features centric. `duckdb-3d` adds the
solid-aware types and functions that 3D workflows need, without pulling in a heavyweight
geometry backend.

> Status: the core is implemented and tested. It is not CityJSON-specific — CityJSON is one
> upstream producer, consumed through plain SQL composition with the
> [`cityjson`](https://github.com/cityjson/duckdb-cityjson) extension.

## Highlights

- **`SOLID_3D`** — a dedicated type for closed polyhedral solids, backed by a compact,
  versioned binary payload that preserves shell/face topology (triangulation is a derived
  cache, never the source of truth).
- **`GEOM_3D`** — a general 3D geometry type (points, lines, polygons, multis, polyhedral
  surfaces) for the class-generic accessor, distance, and serialization functions.
- **Validation, not repair** — `duckdb-3d` reports closedness, manifoldness, orientation, and
  degeneracy; it never silently "fixes" geometry.
- **Familiar SQL** — PostGIS-style `ST_*` / `ST_3D*` names (a curated 3D subset, not full
  PostGIS parity).
- **Self-contained kernel** — no CGAL/SFCGAL dependency; the current function set runs on a
  pure C++ geometry kernel.

## Quick start

Build the extension (see [Building](#building)), then in the DuckDB shell:

```sql
LOAD three_d;

-- Build a solid from WKB (e.g. produced by the cityjson extension), then measure it.
SELECT ST_3DVolume(solid)          AS volume_m3,
       ST_ZMax(solid) - ST_ZMin(solid) AS height_m,
       ST_3DIsClosed(solid)        AS closed
FROM (SELECT ST_3DFromWKB(geometry, geometry_properties) AS solid FROM my_buildings);
```

Reading real 3D buildings straight from a remote CityJSONSeq server, with the `cityjson`
extension:

```sql
INSTALL cityjson FROM community;   -- one-time
LOAD cityjson;
LOAD three_d;

SELECT id,
       ROUND(ST_3DVolume(solid), 1) AS volume_m3
FROM (
  SELECT id, ST_3DFromWKB(geometry, geometry_properties) AS solid
  FROM read_cityjsonseq(
    'https://storage.googleapis.com/cityjson/delft.city.jsonl', lod => '2.2')
  WHERE geometry IS NOT NULL
)
WHERE ST_3DValidationReport(solid).is_valid
LIMIT 10;
```

A full, ground-truth-validated walkthrough against the 3DBAG Delft dataset is in
**[docs/EXAMPLE.md](docs/EXAMPLE.md)**.

## Function reference

All functions use PostGIS-style names. `SOLID_3D`-only functions take the solid payload;
class-generic ones also accept `GEOM_3D` (built with `ST_Geom3DFromWKB`).

| Category | Functions |
| --- | --- |
| **Import / export** | `ST_3DFromWKB`, `ST_3DTryFromWKB`, `ST_3DAsWKB`, `ST_Geom3DFromWKB`, `ST_AsText`, `ST_AsGeoJSON`, `ST_AsBinary` |
| **Introspection** | `ST_3DBounds`, `ST_3DNumSolids`, `ST_3DNumShells`, `ST_3DNumFaces`, `ST_GeometryType`, `ST_NDims`, `ST_HasZ`, `ST_CoordDim`, `ST_Dimension`, `ST_NumGeometries`, `ST_X`, `ST_Y`, `ST_Z`, `ST_ZMin`, `ST_ZMax` |
| **Validation** | `ST_3DIsClosed`, `ST_3DIsManifold`, `ST_3DIsOriented`, `ST_3DValidationReport`, `ST_IsPlanar` |
| **Measurement** | `ST_3DVolume`, `ST_3DSurfaceArea` / `ST_3DArea`, `ST_Area` (footprint), `ST_3DPerimeter`, `ST_3DLength` |
| **Distance** | `ST_3DDistance`, `ST_3DDWithin`, `ST_3DMaxDistance`, `ST_3DDFullyWithin`, `ST_3DIntersects`, `ST_3DClosestPoint`, `ST_3DShortestLine` |
| **Transform / construct** | `ST_Translate`, `ST_Scale`, `ST_RotateX`, `ST_RotateY`, `ST_RotateZ`, `ST_Force3D`, `ST_ConvexHull`, `ST_3DCentroid`, `ST_3DExtrude`, `ST_MakeSolid` |

Signatures, preconditions, and the binary payload format are specified in
[docs/DESIGN_DOC.md](docs/DESIGN_DOC.md).

## Building

The extension targets DuckDB `v1.5.x` and builds with the standard DuckDB extension
toolchain. Clone with submodules (DuckDB is a submodule), then build:

```sh
git clone --recurse-submodules <repo-url>
cd duckdb-3d
make                 # first build compiles DuckDB too; subsequent builds are incremental
```

For much faster incremental builds, install [ccache](https://ccache.dev) and
[ninja](https://ninja-build.org):

```sh
GEN=ninja make
```

Artifacts:

- `build/release/duckdb` — a DuckDB shell with `three_d` preloaded
- `build/release/extension/three_d/three_d.duckdb_extension` — the loadable extension

## Testing

The project follows strict test-driven development (see [AGENTS.md](AGENTS.md)).

```sh
make test          # SQL tests against the release build
make test_debug    # SQL + C++ tests against the debug build
```

- SQL tests live in `test/sql/`, C++ kernel tests in `test/cpp/`.
- The CityJSON interop tests (`test/sql/cityjson_interop.test`,
  `test/sql/cityjson_delft_remote.test`) are gated on the `cityjson` community extension and
  skip automatically when it is not registered with the test runner — see
  [docs/CITYJSON_INTEROP.md](docs/CITYJSON_INTEROP.md) to run them.

## Documentation

| Document | Contents |
| --- | --- |
| [docs/EXAMPLE.md](docs/EXAMPLE.md) | Hands-on walkthrough against real 3DBAG data |
| [docs/DESIGN_DOC.md](docs/DESIGN_DOC.md) | Reference spec: SQL contract, `SOLID_3D` payload, validation & measurement semantics, roadmap |
| [docs/CITYJSON_INTEROP.md](docs/CITYJSON_INTEROP.md) | Composing with the `cityjson` extension; running the interop tests |
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
