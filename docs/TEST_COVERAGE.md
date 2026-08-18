# Test Coverage

What the suites cover, and what each oracle is authoritative for.

Suite layout and the TDD workflow are in [AGENTS.md](../AGENTS.md); this file records the
**verification strategy** — which independent source each claim is checked against.

## Suites

| Suite | Runs | Covers |
| --- | --- | --- |
| `test/cpp/` | `make test_debug`, `make test_cpp` | Kernel logic with no database: WKB parsing, model construction, payload round-trips, validation, triangulation, area/volume math |
| `test/sql/` | `make test`, `make test_debug` | SQL surface: binding, null propagation, `TRY` semantics, result contracts, interop |

Tests gated on `require cityjson` skip unless the community extension is registered with the
sqllogic runner — see [CITYJSON_INTEROP.md](./CITYJSON_INTEROP.md). Tests declaring
`require-env THREE_D_TEST_FIXTURES` skip unless that variable is exported; the `Makefile`
exports it.

## Oracles

Three independent sources, each authoritative for a different class of claim.

### 3DBAG published attributes — real-geometry measurement

`test/sql/cityjson_delft_remote.test` streams the 3DBAG Delft tile and compares
`ST_3DVolume` against `b3_volume_lod22` and `ST_3DFootprintArea` against `b3_opp_grond`.
This is the oracle of record for measurement on **real reconstructed geometry**, because
SFCGAL rejects most real roofs for non-planarity while this extension measures them by
triangulation.

Requires network access and the `cityjson` extension.

### PostGIS + SFCGAL — analytic measurement math

A differential harness, and **never** a build, runtime, or CI dependency.
`scripts/oracle/gen_golden.py` feeds identical WKB bytes to both engines offline and freezes
SFCGAL's answers into `test/data/postgis_oracle/`; `test/sql/postgis_oracle.test` replays
those frozen values with no PostGIS present. Feeding the same bytes to both isolates the math
from ingestion differences.

Golden values carry their provenance (`pg_version`, `sfcgal` version). Regenerating against a
different PostGIS/SFCGAL build churns every value, so it must be run on the provisioned
container.

Where PostGIS is *not* a valid oracle:

| Functions | Valid? | Why |
| --- | --- | --- |
| Accessors, affine transforms, serialization | Yes | Direct analogues; not all are wired into the generator yet |
| `ST_3DPerimeter`, `ST_3DCentroid` | Partly | Analogues exist, but the centroid definition must be matched deliberately |
| `ST_3DNumSolids/Shells/Faces`, `ST_3DBounds` | Weak | Shell and patch counting differ |
| `ST_3DValidationReport`, `ST_3DIsManifold`, `ST_3DIsOriented` | **No** | PostGIS repairs or rejects; this extension flags without repairing. Its own report is authoritative |

### Metamorphic properties — dependency-free invariants

`test/sql/metamorphic_transforms.test` asserts relationships that hold regardless of any
external engine: translation and rotation preserve volume and area, scaling follows the cube
law. This covers the transform family, which the differential oracle structurally cannot.

## Notable fixtures

| Fixture | Pins |
| --- | --- |
| `ST_AsWKBHollowCube()` + `test/cpp/test_inner_shell.cpp` | Interior-shell subtraction (volume 56, area 120), shell-grouping invariance, and rejection of a same-wound cavity |
| `ST_AsWKBMultiCube()` | Collection-of-solids math in every CI run, ungated |
| `test/data/multisolid.city.json`, `compositesolid.city.json` | `MultiSolid` / `CompositeSolid` import with per-solid shell grouping (`test/sql/cityjson_multisolid.test`) |
| `test/data/unit_cube.city.json` | End-to-end `cityjson` → `three_d` smoke test |

## Open work

**Expand the PostGIS oracle** to the "Yes" rows above — accessors, affine transforms, and
serialization — for the existing fixtures and accepted 3DBAG geometry. Highest value first:
3D bounds and Z-extent, which compare exactly on every row including SFCGAL-rejected roofs.
This requires editing `scripts/oracle/gen_golden.py` and regenerating `golden.csv` on the
provisioned PostGIS + SFCGAL container so the frozen provenance stays consistent.
