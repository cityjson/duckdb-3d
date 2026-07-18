# Design: `ST_Transform` (2D CRS reprojection)

**Status:** approved, in implementation
**Date:** 2026-07-18
**Related:** [FUTURE_WORK.md §3](../../FUTURE_WORK.md#3-coordinate-reference-system-support-st_transform-srid),
[DESIGN_DOC.md §6.3 (coordinate semantics)](../../DESIGN_DOC.md)

## Goal

Add `ST_Transform` to reproject geometry between coordinate reference systems, matching
PostGIS's **horizontal-only** default: X/Y are reprojected, **Z is passed through unchanged**
(no vertical datum / geoid transformation). This lifts the "Cartesian, no CRS" limitation
documented in FUTURE_WORK §3, option 2.

## Scope

- **In:** `ST_Transform` on `SOLID_3D` and `GEOM_3D`; EPSG-integer and CRS-string signatures;
  XY reprojection via PROJ; Z preserved; bbox recomputed; solids re-validated.
- **Out (deliberately):** vertical datum transforms; storing an SRID in the payload
  (CRS is always explicit in the call); a `TRY` variant; single-file distribution bundling
  of `proj.db` (staged as a follow-up — see Risks).

## Semantics

1. **2D only.** For each vertex, PROJ transforms `(x, y)`; the original `z` is reattached
   verbatim. Even if a supplied CRS pair implies a 3D pipeline, only XY is taken.
2. **Axis order normalised** to easting/northing (lon/lat) via
   `proj_normalize_for_visualization`, so `EPSG:4326` behaves as `(lon, lat)` — the GIS /
   PostGIS convention, not the authority's declared lat/lon order.
3. **Type-preserving.** Output type equals input type (`SOLID_3D → SOLID_3D`,
   `GEOM_3D → GEOM_3D`).
4. **NULL propagation** on any NULL argument (geometry or either CRS).

## Signatures

```
ST_Transform(geom, source_srid INTEGER, target_srid INTEGER) → same type
ST_Transform(geom, source_crs VARCHAR, target_crs VARCHAR)   → same type
```

The integer form formats `EPSG:<n>` (rejecting `srid <= 0`) and delegates to the string
form. The string form accepts a CRS description in each position — an authority code
(`'EPSG:28992'`) or a WKT2 CRS string. Coordinate-operation **pipeline** strings
(`'+proj=pipeline ...'`) are *not* accepted, because `proj_create_crs_to_crs` requires a CRS
(not an operation) in each argument; supporting source-position pipelines would need a
different PROJ entry point and is left as a possible future enhancement. Each signature is
registered as a `SOLID_3D` overload, a `GEOM_3D` overload, and a `BLOB` overload (the last so
raw `ST_3DFromWKB` output, which is typed `BLOB`, binds).

## Architecture — PROJ isolated to one kernel unit

New unit **`src/kernel/crs_transform.{hpp,cpp}`** is the *only* file that includes `proj.h`.
`proj.h` does not leak into the header (opaque `void*` handles). Interface:

```cpp
namespace duckdb_3d {
  // "EPSG:<srid>"; throws on srid <= 0.
  std::string EpsgToAuthString(int32_t srid);

  // Wraps a PROJ context + normalized transform. Non-copyable.
  class CrsTransform {
  public:
    CrsTransform(const std::string &source_crs, const std::string &target_crs); // throws on invalid CRS
    ~CrsTransform();
    // Reproject XY of every vertex in place; Z untouched. Throws on transform failure.
    void ReprojectXY(std::vector<Vertex3D> &vertices) const;
  };
}
```

The SQL layer (`src/three_d_extension.cpp`) decodes each row's payload, calls
`ReprojectXY(model.vertices)`, fixes the caches, and re-serialises. Nothing else in the
kernel learns about CRS. Within a chunk, one `CrsTransform` is built per distinct
`(source, target)` pair and reused across rows (constant-CRS is the common case).

## Cache handling after reprojection

Reprojection is nonlinear, so on the decoded model:

- **bbox** — always recomputed via `ComputeBBox()`.
- **topology offsets + triangulation indices** — preserved unchanged (index-based; smooth
  reprojection keeps faces valid). Area/volume recompute correctly from moved vertices.
- **`SOLID_3D` validation flags** — re-run `ValidateSolidModel`, because a
  handedness-flipping CRS can invert winding; we don't trust the pre-transform `is_oriented`.
- **`GEOM_3D`** — no validation cache to refresh; bbox only.

## Errors

Unknown/invalid CRS, axis-normalisation failure, or a per-point PROJ transform error →
descriptive `std::runtime_error` surfaced as a DuckDB exception (non-`TRY`, consistent with
the "fail clearly, no silent repair" contract). `srid <= 0` → error.

## Build / dependency

- Add `proj` to `vcpkg.json` (CI builds it statically like other extension deps).
- `CMakeLists.txt` and `test/cpp/CMakeLists.txt`: `find_package(PROJ CONFIG)` and link
  `PROJ::proj` into the static extension, the loadable extension, and the C++ test binary.
  On macOS, add Homebrew prefixes to `CMAKE_PREFIX_PATH` so local (non-vcpkg) builds resolve
  PROJ; under the vcpkg toolchain the vcpkg copy is found first.

## TDD plan

C++ (fast target, `make test_cpp`) — `test/cpp/test_crs_transform.cpp`:
- `EPSG:4326 → EPSG:3857` on a known point (e.g. `(0,0) → (0,0)`; a mid-latitude point to a
  known easting/northing) within tolerance;
- **Z preserved exactly** through a transform;
- **round-trip** A→B→A within epsilon;
- invalid CRS throws; `EpsgToAuthString(0)` throws.

SQL (`test/sql/st_transform.test`):
- both signatures bind and return the input type;
- NULL geom / NULL CRS → NULL;
- a metric→metric transform leaves a solid's **volume unchanged** (sanity) while bbox moves;
- unknown SRID raises.

## Risks

- **`proj.db` at runtime.** PROJ needs its datum database. Locally it resolves via the
  Homebrew/vcpkg install path; bundling it into a single distributable
  `.duckdb_extension` (as `duckdb_spatial` does, via `proj_context_set_search_paths`) is a
  **follow-up**, agreed with the user. First cut targets green local tests.
- **CI build weight.** Building PROJ + deps under vcpkg lengthens CI; acceptable and expected
  for CRS support.
