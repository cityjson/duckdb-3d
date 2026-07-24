# Test Coverage Audit

Snapshot of test coverage for the `duckdb-3d` extension, and the plan for closing
gaps. Generated as part of the test-comprehensiveness pass.

## Method

- **Function inventory:** every `ST_*` name registered in `src/three_d_extension.cpp`
  (60 names, counting the `st_aswkb*` SQL test-helpers).
- **SQL coverage:** whether the name appears in any `test/sql/*.test` (excluding the oracle).
- **Oracle coverage:** whether the function is cross-checked in the PostGIS/SFCGAL differential
  oracle (`test/sql/postgis_oracle.test` + `scripts/oracle/gen_golden.py`).
- **C++ coverage:** whether the backing kernel unit has a dedicated `test/cpp/test_*.cpp`.

## Headline results

- **SQL integration: 60 / 60 functions have at least one SQL test.** No function is entirely
  untested at the SQL level.
- **PostGIS oracle: partial by design.** The oracle covers the measurement/distance core
  (area, volume, surface area, `ST_3DDistance` family, intersects, closest/shortest point,
  length, `ST_IsClosed`, convex-hull area). It does **not** cover accessors, transforms,
  introspection counts, perimeter, centroid, or serialization — see gaps below.
- **C++ kernel: most units have direct tests.** Direct-test gaps: `wkb_export`,
  `triangulation` (both currently covered only indirectly).

## What this pass added

| Area | Added |
|---|---|
| Inner shells (Gap 1) | `test/cpp/test_inner_shell.cpp` (hollow cube, vol 56 / area 120; shell-grouping invariance; §9.3 gap pin) + `test/sql/st_3d_hollow_solid.test` via new `ST_AsWKBHollowCube()` helper. **Done.** |
| Multi-solid (Gap 3) | `ST_AsWKBMultiCube()` + `test/sql/st_3d_multisolid.test` (ungated); real `msol`/`csol` datasets + `test/sql/cityjson_multisolid.test` (gated, verified against live cityjson). **Done.** |
| Transform oracle (part of Gap 2) | `test/sql/metamorphic_transforms.test` — dependency-free invariance (translation/rotation preserve volume+area; scaling laws) over controlled fixtures **and** real 3DBAG golden solids. Covers the transform family the PostGIS oracle structurally cannot. **Done.** |
| C++ units (Gap 4) | `test/cpp/test_wkb_export.cpp` (round-trip) and `test/cpp/test_triangulation.cpp` (quad, n-gon, concave L-face, tilted face). **Done.** |
| Docs | This file; `DESIGN_DOC §10.2.1` corrected (Finding B); `FUTURE_WORK §4` (cross-shell orientation check). |

C++ suite 153/153; SQL suite green (cityjson + oracle-container tests skip locally).

## Gap 1 — Inner-shell (hollow solid) end-to-end test is missing

The most material gap. The signed-volume subtraction for a solid with an interior shell
(cavity) — documented in [DESIGN_DOC §10.2.1](./DESIGN_DOC.md#1021-interior-inner-shell-handling--mechanism-and-rationale)
— is **not** exercised end-to-end:

- `test/cpp/test_metadata.cpp::MakeTwoShellWKB` builds a 2-shell solid but asserts only
  `ShellCount()==2` / `FaceCount()==8` — it never validates the model or checks volume.
- The SQL multi-shell cases (`test/sql/st_3d_metadata.test`) are all **error** cases
  (conflicting `shellFaceCounts`, malformed JSON). There is no positive hollow-solid test.

**Plan:** add a hollow-solid fixture with a known analytic volume (V_outer − V_inner) and test
it at both cpp (`ComputeVolume` passes closed/manifold/oriented preconditions and returns the
expected value) and SQL levels. No PostGIS needed.

## Gap 2 — PostGIS oracle does not cover all functions

Functions **not** in the oracle, split by whether PostGIS is a *valid* oracle for them:

| Not oracled | PostGIS a valid oracle? | Note |
|---|---|---|
| `ST_3DX/Y/Z`, `ST_3DZMin/ZMax`, `ST_NDims`, `ST_CoordDim`, `ST_3DDimension`, `ST_3DNumGeometries` | **Yes** | Direct PostGIS analogues; add to generator. |
| `ST_3DTranslate`, `ST_3DScale`, `ST_3DRotateX/Y/Z` | **Yes** | PostGIS computes identical affine maps. |
| `ST_3DTransform` | **Yes** (with PROJ) | PostGIS `ST_Transform`; 2D only here. |
| `ST_3DPerimeter`, `ST_3DCentroid` | Partly | Analogues exist; centroid definition must match. |
| `ST_3DAsText`, `ST_3DAsBinary`, `ST_3DAsGeoJSON` | **Yes** | WKT/WKB/GeoJSON serialisation. |
| `ST_3DNumSolids/Shells/Faces`, `ST_3DBounds` | Weak | Shell/patch counting differs; PostGIS not a clean oracle. |
| `ST_3DValidationReport`, `ST_3DIsManifold`, `ST_3DIsOriented` | **No** | three_d-specific "fail clearly, no repair" semantics; PostGIS repairs/rejects (design doc §9.5.1). three_d's own report is the oracle of record. |

**Status — DEFERRED (needs your environment).** The oracle is a *differential* harness: its
golden values are produced by a live PostGIS + SFCGAL container (`just oracle-regen`), frozen
with provenance (`pg_version 3.4.3`, `sfcgal 1.3.8`). This environment has no running Docker /
PostGIS, and regenerating with a *different* PostGIS/SFCGAL version would churn every golden
value — defeating the frozen-file design. Expanding the oracle therefore needs to be run on
the maintainer's provisioned `pg_oracle` container. Concrete turnkey plan for when it is:

1. Extend `scripts/oracle/gen_golden.py` to emit the "Yes"-row columns above (accessors,
   affine transforms, serialisation) for the existing fixtures + accepted 3DBAG geometry.
2. Add the matching assertions to `test/sql/postgis_oracle.test`.
3. `just oracle-regen` on the container, commit the refreshed `golden.csv`.

## Gap 3 — Real multisolid / compositesolid datasets

**Done.** The CityJSON `msol` (MultiSolid) and `csol` (CompositeSolid) example datasets from
<https://www.cityjson.org/datasets/#simple-geometries> are committed as
`test/data/multisolid.city.json` / `compositesolid.city.json` and exercised by
`test/sql/cityjson_multisolid.test` (gated `require cityjson`). Because metadata-aware import
**raises** for `MultiSolid`/`CompositeSolid` (`model_builder.cpp`; interior-shell grouping is
deferred, [FUTURE_WORK §1](./FUTURE_WORK.md#1-composite--multi-solid-support-with-interior-shells)),
the test pins both the supported fallback (plain path → 2 solids, volume 2.0, area 12.0,
closed) and the documented boundary (metadata import raises; TRY → NULL). An ungated
`ST_AsWKBMultiCube()` covers the collection-of-solids maths in every CI run.

## Gap 4 — C++ direct-test gaps — **Done**

- `wkb_export` — now `test/cpp/test_wkb_export.cpp` (model → WKB → model round-trip: counts,
  bbox, volume; PolyhedralSurface type code).
- `triangulation` — now `test/cpp/test_triangulation.cpp` (convex quad, n-gon count, concave
  L-face area via ear-clipping, tilted-plane true area).

## Remaining deferrals (need maintainer action)

1. **Oracle golden expansion (Gap 2 core).** Adding accessor / bounds / serialisation columns
   to the PostGIS oracle requires editing `scripts/oracle/gen_golden.py` **and** regenerating
   `golden.csv` on the provisioned PostGIS+SFCGAL container (`just oracle-regen`) to keep the
   frozen provenance (`pg_version 3.4.3`, `sfcgal 1.3.8`) consistent. Not possible in this
   environment (no Docker/PostGIS). Highest-value first addition: 3D bounds / Z-extent columns
   (`ST_3DZMin/ZMax/3DBounds`) — exact comparison, works on every row including SFCGAL-rejected
   roofs. The transform family is already covered dependency-free by
   `metamorphic_transforms.test`, so the oracle need not.
2. **Cross-shell orientation check** ([FUTURE_WORK §4](./FUTURE_WORK.md#4-enforce-the-interior-opposite-exterior-orientation-invariant))
   — a kernel behaviour change (validation), not just a test. The "same-wound interior shell"
   case in `test_inner_shell.cpp` is pre-written to flip into its regression test.
