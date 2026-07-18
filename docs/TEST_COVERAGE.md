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
| `ST_X/Y/Z`, `ST_ZMin/ZMax`, `ST_NDims`, `ST_CoordDim`, `ST_Dimension`, `ST_NumGeometries` | **Yes** | Direct PostGIS analogues; add to generator. |
| `ST_Translate`, `ST_Scale`, `ST_RotateX/Y/Z` | **Yes** | PostGIS computes identical affine maps. |
| `ST_Transform` | **Yes** (with PROJ) | PostGIS `ST_Transform`; 2D only here. |
| `ST_3DPerimeter`, `ST_3DCentroid` | Partly | Analogues exist; centroid definition must match. |
| `ST_AsText`, `ST_AsBinary`, `ST_AsGeoJSON` | **Yes** | WKT/WKB/GeoJSON serialisation. |
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

The CityJSON example datasets (`msol`, `csol`, etc. from
<https://www.cityjson.org/datasets/#simple-geometries>) are not yet used. Note the current
limitation: metadata-aware import **raises** for `MultiSolid`/`CompositeSolid`
(`model_builder.cpp`), so their interior-shell grouping cannot be recovered today (tracked in
[FUTURE_WORK §1](./FUTURE_WORK.md#1-composite--multi-solid-support-with-interior-shells)).
Plan finalised with advisor input — see the remediation section once implemented.

## Gap 4 — C++ direct-test gaps

- `wkb_export` — round-trip export is covered at SQL level (`st_3d_as_wkb.test`) but has no
  dedicated `test/cpp` unit test.
- `triangulation` — covered indirectly through `test_measurements`; no dedicated unit test.

Both are lower priority than Gaps 1–3 but worth closing for kernel-level regression safety.
