# Test Coverage

What the suites cover, and what each oracle is authoritative for.

Suite layout and the TDD workflow are in [AGENTS.md](../AGENTS.md); this file records the
**verification strategy** — which independent source each claim is checked against.

## Suites

| Suite | Runs | Covers |
| --- | --- | --- |
| `test/cpp/` | `make test_cpp`, `make test_all`, `make test_full` | Kernel logic with no database: WKB parsing, model construction, payload round-trips, validation, triangulation, area/volume math |
| `test/sql/` | `make test`, `make test_debug`, `make test_full` (release, gated tests included) | SQL surface: binding, null propagation, `TRY` semantics, result contracts, interop |

Five tests gated on `require cityjson` skip unless a **locally built** `duckdb-cityjson` is
staged for the sqllogic runner — the community-published extension emits an older column
shape these tests no longer target. `make test_full` stages it (and fails the run on any
skip); see [CITYJSON_INTEROP.md](./CITYJSON_INTEROP.md#running-the-gated-tests-under-sqllogic)
for the mechanics. Tests declaring `require-env THREE_D_TEST_FIXTURES` skip unless that
variable is exported; the `Makefile` exports it.

## Oracles

Five independent sources, each authoritative for a different class of claim.

### 3DBAG published attributes — real-geometry measurement

3DBAG publishes, per building, what its own reconstruction pipeline measured. Those figures
are an independent toolchain's answer for the very geometry it shipped, which makes them the
oracle of record for measurement on **real reconstructed geometry** — the case SFCGAL cannot
serve, because it rejects most real roofs for non-planarity while this extension measures
them by triangulation.

| Attribute | Oracles |
| --- | --- |
| `b3_volume_lod12`, `b3_volume_lod13`, `b3_volume_lod22` | `ST_3DVolume`, at each LoD |
| `b3_opp_grond` + `b3_opp_dak_plat` + `b3_opp_dak_schuin` + `b3_opp_buitenmuur` + `b3_opp_scheidingsmuur` | `ST_3DSurfaceArea` / `ST_3DArea` at LoD2.2 — every face of that solid carries exactly one of the five semantic roles, so they sum to its whole boundary |
| `b3_opp_grond` | `ST_3DFootprintArea` — the ground surface is horizontal, so its 3D area and its XY projection are one number |
| `b3_h_maaiveld` | `ST_3DZMin`, at every LoD |
| `b3_h_dak_70p` | `ST_3DZMax` at LoD1.2 — the height the block was extruded to |
| `b3_val3dity_lod22` | `ST_3DValidationReport` — one-way: what val3dity passes, this extension must pass |

Two tests run those comparisons.

- **`test/sql/cityjson_3dbag_attributes.test`** — offline, over the frozen nine-building slice
  in `test/data/3dbag.city.jsonl`, at the tolerances that slice actually meets (0.1 % on
  volume, 1 % on surface area, 2 mm on ZMin) plus string-exact per-building pins. Needs the
  `cityjson` extension but **no network**, so it is the one that runs anywhere.
- **`test/sql/cityjson_delft_remote.test`** — the same comparisons at tile scale, streamed
  from the remote Delft tile. At that scale the thresholds are fractions ("≥95 % of buildings
  within 2 %") rather than bounds, because a tile contains reconstructions that are simply
  broken. Requires network access.

The val3dity cross-check is a fraction, not a bound, and deliberately: the two disagree on a
handful of tile solids that carry a single collapsed face. val3dity tolerates it; this
extension reports the duplicated edge it creates as non-manifold and refuses to measure the
solid, which is the documented no-silent-repair behaviour rather than a defect.

Numeric pins in both files are formatted with `printf` and compared as text. sqllogic compares
numeric results with a **1 % relative tolerance**, so a bare `ROUND(...)` pin admits drift an
order of magnitude larger than the decimals it displays.

### PostGIS + SFCGAL — analytic measurement math

A differential harness, and **never** a build, runtime, or CI dependency.
`scripts/oracle/gen_golden.py` feeds identical WKB bytes to both engines offline and freezes
PostGIS's answers into `test/data/postgis_oracle/` (three CSVs: per-geometry, per-pair,
per-transform); `test/sql/postgis_oracle.test` replays those frozen values with no PostGIS
present. Feeding the same bytes to both isolates the math from ingestion differences.

The input set has two roles. `geom_role = 'solid'` rows import through `ST_3DFromWKB` (the two
tetra fixtures, the hollow cube, the two-member cube collection in an adjacent and a
far-separated variant, nine 3DBAG solids); `geom_role = 'geom'` rows are one fixture per `GEOM_3D` class — point, line, multi-line,
polygon, warped polygon, multi-point, multi-polygon — which is what makes the accessor and
serialization columns non-trivial.

Golden values carry their provenance (`pg_version`, `sfcgal` version). Regenerating against a
different PostGIS/SFCGAL build churns every value, so it must be run on the provisioned
container. `test/data/postgis_oracle/README.md` documents every column.

#### What the oracle covers

| PostGIS reference | Oracles |
| --- | --- |
| `ST_3DArea`, `ST_Volume(ST_MakeSolid(·))` | `ST_3DSurfaceArea` / `ST_3DArea`, `ST_3DVolume` |
| `ST_IsClosed` | `ST_3DIsClosed` |
| `ST_Area(ST_ConvexHull(ST_Points(·)))` | `ST_3DFootprintHull` (via `ST_3DFootprintArea`) |
| `ST_Area(ST_Force2D(·))` | `ST_3DFootprintArea` (halved on two-sided classes) |
| `ST_XMin`…`ST_ZMax` | `ST_3DBounds`, `ST_3DZMin`, `ST_3DZMax` |
| `ST_NDims`, `ST_CoordDim`, `ST_GeometryType` | `ST_NDims`, `ST_CoordDim`, `ST_3DGeometryType` |
| `ST_Dimension`, `ST_NumGeometries` | `ST_3DDimension`, `ST_3DNumGeometries` (on the `geom` rows — see below) |
| `ST_3DLength` | `ST_3DLength` |
| `ST_X`/`ST_Y`/`ST_Z` | `ST_3DX` / `ST_3DY` / `ST_3DZ` (the Point fixture) |
| SFCGAL `ST_IsPlanar` | `ST_IsPlanar` (the two Polygon fixtures) |
| `ST_AsBinary`, `ST_AsText`, `ST_AsGeoJSON` | `ST_3DAsWKB`, `ST_3DAsBinary`, `ST_3DAsText`, `ST_3DAsGeoJSON` |
| `ST_3DDistance`/`MaxDistance`/`Intersects`/`DWithin`/`DFullyWithin`, `ST_3DShortestLine`, `ST_3DClosestPoint` | the whole distance family |
| `ST_Translate`, `ST_Scale`, `ST_RotateX/Y/Z`, `ST_Transform` | `ST_3DTranslate`, `ST_3DScale`, `ST_3DRotateX/Y/Z`, `ST_3DTransform` |

Three comparisons are deliberately indirect:

- **Geometry-returning functions are compared through what they measure.** Transforms
  through the transformed bounding box and a re-measurement of volume/area/length;
  `ST_3DShortestLine` and `ST_3DClosestPoint` through their length and distance-to-`b`. The
  live comparison below compares `ST_3DGeometryN` and `ST_3DGeomFromText` byte for byte.
- **WKT is compared numerically, not as a string.** `kernel/geom_serialize.cpp` formats
  ordinates with `%.9g` while PostGIS emits round-trip precision, and PostGIS writes no space
  after the comma. The test extracts the numbers from both and compares them pairwise, token
  count included.
- **`ST_3DTransform`'s reprojected area and volume are not compared.** They would be in
  degrees, and the ~1e-9° pipeline difference between the two PROJ builds amplifies into a
  1e-3 relative gap on the smallest faces. The bounding box is the oracle there.

#### Where PostGIS is *not* the reference

| Functions | Why not | What covers them instead |
| --- | --- | --- |
| `ST_3DPerimeter` | PostGIS's `ST_3DPerimeter` sums *every* face's ring length; this extension's sums the **boundary** edges only (0 for a closed solid). Two different measurements | `docs/TESTING.md` §10–11 on the nine unclosed 3DBAG parts, `st_3d_measurements.test` |
| `ST_3DCentroid` | PostGIS has no 3D centroid. `ST_Centroid` raises outright on a `PolyhedralSurface`, and on the other classes it is a 2D, XY-projected-area-weighted point — not this extension's 3D-area-weighted `Point Z` | `geom_3d_construct.test`, `docs/TESTING.md` §6 and §15 |
| `ST_3DDimension`, `ST_3DNumGeometries` on a `PolyhedralSurface` | PostGIS reads a *closed* `PolyhedralSurface` as a solid (`ST_Dimension` = 3, and 2 for an open one) and `ST_NumGeometries` as its patch count. This extension reports the topological dimension of the surface (2) and treats the surface as one geometry | oracled on the `geom` rows; `st_3d_introspection.test`, `geom_3d_accessors.test` |
| `ST_3DNumSolids`, `ST_3DNumShells`, `ST_3DNumFaces` | Shell and patch counting differ structurally: the `shells` sidecar partition is a CityGML concept PostGIS's flat `PolyhedralSurface` has no equivalent of | `test/cpp/test_inner_shell.cpp`, `st_3d_hollow_solid.test`, `st_3d_multisolid.test`, `cityjson_multisolid.test` |
| `ST_3DValidationReport`, `ST_3DIsManifold`, `ST_3DIsOriented` | PostGIS repairs or rejects; this extension flags without repairing | val3dity (above), `st_3d_validation.test`, `test/cpp/test_validation.cpp`, `docs/TESTING.md` §8–9 |
| The distance family on faces with holes or non-planar faces | See the live comparison above | `live_diff.py`'s independent computation; `geom_3d_distance.test`, `test/cpp/test_geom_distance.cpp` |
| `ST_3DFromWKB`, `ST_3DTryFromWKB`, `ST_Geom3DFromWKB`, `ST_MakeSolid`, `ST_3DExtrude`, `ST_Force3D`, `ST_3DHasZ` | Import and construction contracts, with no PostGIS analogue. Their correctness is proven *downstream*: every oracled measurement above runs on geometry they produced, so a broken importer fails the area/volume/bbox comparisons | `st_3d_from_wkb*.test`, `cityparquet_round_trip.test`, `geom_3d_construct.test`, `test/cpp/` |

### val3dity — validity

[val3dity](https://github.com/tudelft3d/val3dity), built from source and run with its
defaults (1 mm snapping, 1 cm planarity tolerance), is the reference for what
`ST_3DValidationReport` decides. It is a development-time comparison, not a CI step. On the
3DBAG Delft tile it agrees with `is_valid` on all 1116 LoD1.2 solids and on 1109 of the 1116
LoD2.2 solids. The seven LoD2.2 differences are documented tolerances rather than missing
checks: one non-planar roof (duckdb-3d does not test planarity), five solids whose faces
collapse under 1 mm snapping (duckdb-3d does not snap), and one repeated consecutive vertex,
which import drops (DESIGN_DOC §2.1). The checks val3dity has and this extension added —
connectivity (305), non-manifold vertices (303), self-intersection (306, 401), cavities
outside the exterior or inside each other (403, 401) — have no disagreement. The kernel
tests in `test/cpp/test_validation.cpp` pin each check on constructed solids and on real
3DBAG buildings, including that the verdict survives rotation and a change of unit.

### Live comparison on real data — distance, solids, measures and elements

`scripts/oracle/live_diff.py` runs the distance family on five published datasets (3DBAG
Delft, the TU Delft LoD3 railway model, Rotterdam Delfshaven, Den Haag, Vienna) in duckdb-3d
and a live PostGIS, and reports every disagreement: object pairs, face pairs and random
points around faces, at thresholds 0, 0.5 and 2. Like `gen_golden.py`, it is a development
tool. Each row is compared against the reference that is right for it:

| Rows | Reference |
| --- | --- |
| Planar faces, no holes | PostGIS, to 1e-6 m |
| Faces with holes | An independent computation in the script: rings projected onto the face's plane, even-odd containment, vertex and edge distances |
| Non-planar faces | The same computation, within twice the faces' deviation from their planes; PostGIS reported beside it for information |
| `ST_3DClosestPoint`, `ST_3DShortestLine` | Their definitions: the point lies on the first geometry, at the distance from the second |

PostGIS is not the reference for the last three rows: its 3D point-in-polygon test ignores
interior rings in the face's plane (a point at the centre of a 1 x 1 hole is 2.12 from the
face, not 0.5), and it measures a non-planar face against a plane of its own fitting, which
on Delft puts some faces 0.7 mm from planar 5 mm out. `test/sql/geom_3d_distance.test` pins
both by construction.

`--checks solids` compares the `SOLID_3D` overloads, which measure the volume, against
SFCGAL's volume predicates on points and object pairs. `--checks measures` compares:

| Function | Reference |
| --- | --- |
| `ST_3DArea(GEOM_3D)` | SFCGAL `ST_3DArea` per face, and each ring's Newell vector area on every row |
| `ST_3DPerimeter(GEOM_3D)` | PostGIS on polygons; on surfaces the edges one patch uses (DESIGN_DOC §8.7) |
| `ST_3DNumGeometries(GEOM_3D)` | PostGIS `ST_NumGeometries` |
| `ST_3DCentroid(SOLID_3D)` | Signed tetrahedra over ear-clipped faces, within how far a different diagonal on a non-planar face moves the centroid |

On the five datasets (seed 20261015) every row agrees: 82 047 faces against SFCGAL and
105 087 geometries against the Newell area, 95 477 polygon and 9 610 surface perimeters,
105 087 part counts and 6 226 solid centroids. SFCGAL is handed each face starting at a vertex
whose successors are not collinear: it takes the plane from the first three vertices, and on
eight Vienna wall faces whose first three are collinear it reports area 0. Two Den Haag
centroids differ from the oracle by up to 1.5 cm only through the diagonal of two non-planar
quads; with the same diagonals the two agree to 1e-14. On 4 103 of the 6 226 solids the faces
are planar enough that the centroid is held to 1e-6 m; the largest difference elsewhere is
7.6 cm, on a Den Haag building whose faces lie up to 30 cm from planar.

`--checks elements` compares what takes a value apart or builds one from text:

| Function | Reference |
| --- | --- |
| `ST_3DXMin` … `ST_3DZMax`, `ST_3DExtent` | PostGIS `ST_XMin` … `ST_ZMax`, `ST_3DExtent` |
| `ST_3DGeometryN(GEOM_3D)` | PostGIS `ST_GeometryN`, byte for byte as WKB |
| `ST_3DGeomFromText` | PostGIS `ST_GeomFromText` of the same text, as WKB |
| CRS from EWKT and EWKB | PostGIS `ST_AsEWKT` / `ST_AsEWKB` of `ST_SetSRID` |
| `ST_3DFaces`, `ST_3DExteriorShell` | PostGIS `ST_GeometryN` of the solid's surface after `ST_RemoveRepeatedPoints`, as WKB |
| `ST_3DFaceAreas(SOLID_3D)` | SFCGAL `ST_3DArea` per face; each ring's Newell vector area on the faces SFCGAL refuses |
| `ST_3DFaceSlopes`, `ST_3DFaceAzimuths` | Their definition, from the exterior ring's Newell normal |

On the five datasets (seed 20261015) every row agrees: 105 087 geometries' bounds, parts,
WKT and CRS round trips exact; 6 422 solids' faces and shells byte for byte; 92 311 face areas
against SFCGAL to 1e-10 and 13 122 more against the Newell area; 413 025 face slopes and
azimuths to 1.4e-14 degrees. Skipped rows are the ones the functions refuse or answer NULL by
definition: 11 polygons the import cannot triangulate, 16 solids with a ring of fewer than
three distinct vertices, and faces with no triangles.

### Metamorphic properties — dependency-free invariants

`test/sql/metamorphic_transforms.test` asserts relationships that hold regardless of any
external engine: translation and rotation preserve volume and area, scaling follows the cube
law. The differential oracle now covers the transform family too, but only through derived
scalars (no geometry round-trip); the metamorphic relations need no engine at all and hold
over the whole frozen 3DBAG corpus.

## Notable fixtures

| Fixture | Pins |
| --- | --- |
| `ST_AsWKBHollowCube()` + `test/cpp/test_inner_shell.cpp` | Interior-shell subtraction (volume 56, area 120), shell-grouping invariance, and rejection of a same-wound cavity |
| `ST_AsWKBMultiCube()` | Collection-of-solids math in every CI run, ungated |
| `ST_AsWKBMultiCube(separation)` | Volume conditioning when a collection's parts are spatially separated — the per-shell reference point of DESIGN_DOC §8.2. Total volume stays 16 at separations of 1e4–1e10, and the 1e6 case is oracled against SFCGAL as `fixture:multi_cube_far` |
| `test/data/multisolid.city.json`, `compositesolid.city.json` | `MultiSolid` / `CompositeSolid` import per solid, with shell grouping recovered from the sidecar (`test/sql/cityjson_multisolid.test`). Import does **not** raise on either class |
| `test/data/unit_cube.city.json` | End-to-end `cityjson` → `three_d` smoke test |
| `test/data/3dbag.city.jsonl` | Nine real 3DBAG buildings at LoD1.2 / 1.3 / 2.2. The WKB the PostGIS oracle freezes as its `NL.IMBAG.Pand.*` rows comes from here, and `test/sql/cityjson_3dbag_attributes.test` measures the same nine against 3DBAG's published attributes offline |
| `test/data/railway_implicit/` | `vegetation.parquet` and `implicit_geometries.parquet` of the CityParquet package `cityparquet convert -o <dir> lib/cityparquet-rs/tests/fixtures/lod3_railway.city.json` writes (a build predating the Parquet JSON logical type for `surfaces` / `other`; the test reads neither, so a regenerated package serves equally): 15 implicit geometries over three relative geometries. `test/sql/st_3d_place_implicit.test` places each and checks its bounds against values computed from the CityJSON source directly (template vertices, matrix and decoded reference vertex), independent of either writer |
| `test/data/3dbag_rs/` | The CityParquet package `cityparquet convert -o <dir> test/data/3dbag.city.jsonl` writes (`cityparquet-rs`). `test/sql/cityparquet_round_trip.test` imports every solid LoD from it, and from a package the `cityjson` extension writes at test time, and requires each solid to export the same WKB and carry the same validation report as the streamed CityJSONSeq, and each LoD0 footprint to keep its area |
| `test/cpp/real_rings.hpp` | Real faces from the 3DBAG Delft tile and the railway fixture — rings pinched at a repeated vertex, a face with four holes, a twisted quad, collinear runs — that `test/cpp/test_triangulation.cpp` requires to triangulate completely, against Newell areas computed independently of any triangulation |

## Open work

- **A hollow/multi-solid reference.** SFCGAL rejects the hollow cube as "not connected", so
  interior-shell subtraction has no differential oracle at all — only the kernel tests and
  the analytic 56/120 pins.
