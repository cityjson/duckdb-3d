# Future Work

Focused, actionable notes on deferred capabilities. This complements the roadmap in
[DESIGN_DOC.md §14 and §16](./DESIGN_DOC.md#14-roadmap-beyond-the-current-surface): §14/§16
enumerate the per-function backlog, while this file records the **larger design decisions**
behind three deferred areas and what "done" would require.

Each item follows the repository's TDD discipline: failing `test/cpp/` + `test/sql/` first,
then implementation, then a design-doc update in the same change.

---

## 1. Composite / Multi-solid Support With Interior Shells — ✅ Done

Implemented via the CityParquet **spec §8 `shells`** key. `geometry_properties`
now carries per-shell emitted-face counts: flat for a `Solid` (`[12]`, `[12,4]`)
and nested per solid for a `MultiSolid`/`CompositeSolid` (`[[12],[8,4]]`).
`src/kernel/metadata_parser.cpp` parses both forms into
`GeometryMetadata.shells` (`vector<vector<uint32_t>>`), and
`BuildSolidModel(surfaces, metadata)` maps one per-shell-count array to each WKB
member — building N solids each with per-solid shell grouping (including interior
shells). The old `requests_multi_solid` guard is gone; a `shells`/member count
mismatch, per-solid face-sum mismatch, or a solid with no non-empty shell is
rejected. Upstream `duckdb-cityjson` emits the `shells` key (its Phase A change).

| Input | Result |
| --- | --- |
| `PolyhedralSurface Z`, plain WKB | one solid, one shell |
| `PolyhedralSurface Z` + `shells [a,b]` | one solid, N shells (interior shells recovered) |
| `GeometryCollection Z`, plain WKB | collection of single-shell solids |
| `GeometryCollection Z` + nested `shells [[…],[…]]` | N solids, per-solid shell grouping incl. cavities |

---

## 2. Move CityJSON-aware Interpretation Out Of `duckdb-3d`

### The concern (separation of concerns)

`duckdb-3d` is meant to be a **CityJSON-agnostic** 3D kernel (DESIGN_DOC §1.2, §2.2;
repo `CLAUDE.md`: *"Keep CityJSON-specific assumptions out of the core kernel"*). Today that
boundary leaks: `src/kernel/metadata_parser.cpp` understands CityJSON-specific fields —
`cityjsonType`, `lod`, `children`, `semantics`, and the CityJSON meaning of `shellCount` —
and `model_builder.cpp` branches on `type == "MultiSolid" / "CompositeSolid"`. That couples
the generic solid kernel to *how CityJSON geometries are interpreted as DuckDB values*, which
is properly the responsibility of the `duckdb-cityjson` extension.

### Proposed direction

Make the shell/solid grouping contract **format-neutral**, so `duckdb-3d` never names
CityJSON:

- **Option A — grouping emitted upstream.** `duckdb-cityjson` resolves CityJSON `Solid` /
  `MultiSolid` / `CompositeSolid` semantics itself and hands `duckdb-3d` a geometry that is
  *already grouped* — e.g. WKB whose structure (or a small neutral sidecar) encodes
  `solid → shell` directly, with no CityJSON vocabulary. `duckdb-3d` consumes a generic
  "solid/shell partition" input and stays ignorant of the source format.
- **Option B — neutral grouping metadata.** Replace the CityJSON-flavoured
  `geometry_properties` keys with a minimal, format-neutral shell-grouping descriptor (counts
  only: solids, shells-per-solid, faces-per-shell). `duckdb-cityjson` translates CityJSON
  structure into that neutral descriptor; `duckdb-3d` parses only the neutral form.

Either way the CityJSON→topology mapping (which shells are interior, LoD selection, semantics)
lives in `duckdb-cityjson`; `duckdb-3d` receives pre-interpreted topology. The
interoperability contract (DESIGN_DOC §12) should be restated in these neutral terms, and
`metadata_parser.cpp` reduced to the neutral schema.

This also cleanly unblocks item 1: multi-solid interior-shell grouping becomes an upstream
concern producing neutral grouping counts, not a CityJSON special case inside the kernel.

---

## 3. Coordinate Reference System Support (`ST_3DTransform`, SRID)

### Status

**`ST_3DTransform` (horizontal / 2D) is implemented** — PROJ-backed reprojection of X/Y with Z
preserved unchanged (no vertical datum), matching PostGIS's default. Signatures:
`ST_3DTransform(geom, source_srid INTEGER, target_srid INTEGER)` and
`ST_3DTransform(geom, source_crs VARCHAR, target_crs VARCHAR)`, on `SOLID_3D` and `GEOM_3D`.
PROJ is confined to `src/kernel/crs_transform.cpp`. See the design at
[docs/superpowers/specs/2026-07-18-st-transform-design.md](./superpowers/specs/2026-07-18-st-transform-design.md).

The base limitation still holds: coordinates remain raw `DOUBLE` XYZ with **no stored SRID**,
so the CRS must be given explicitly on every call, and all *measurement* math (volume, area,
distance) is still Cartesian in the input units. `ST_3DTransform` lets callers reproject into a
suitable metric CRS before those measurements.

### Remaining work

1. **Vertical datum / 3D reprojection.** Currently Z is passed through untouched. Ellipsoidal↔
   orthometric height and geoid models are out of scope; genuinely hard and rarely needed for
   the city-model workflows here. Revisit only on demand.
2. **Stored SRID + `ST_SRID` / `ST_SetSRID`.** Add an SRID field to the `D3DS`/`D3DG` payload
   headers (a versioned change under DESIGN_DOC §7.5). This would enable the single-argument
   `ST_3DTransform(geom, target_srid)` form (reading the stored source SRID, like PostGIS) and
   cross-CRS mismatch detection (e.g. refuse `ST_3DDistance` across differing SRIDs). Lowest
   cost, high safety value — the natural next step.
3. **`proj.db` distribution bundling.** Reprojection depends on PROJ's datum database at
   runtime. Locally it resolves via the Homebrew/vcpkg install path; bundling it into a single
   distributable `.duckdb_extension` (as `duckdb_spatial` does, via
   `proj_context_set_search_paths`) is still outstanding and is the main gap before shipping
   CRS support in a released binary.

---

## 4. Enforce the interior-opposite-exterior orientation invariant — ✅ Done

`CheckInteriorShellWinding` (`src/kernel/validation.cpp`) now enforces §9.3. Per
solid with ≥2 shells it computes each shell's signed volume (origin-translated
tetrahedra sum, so projected-CRS coordinates don't lose a small cavity to
cancellation) and requires every interior shell to be **opposite-signed to, and
smaller in magnitude than, the exterior (shell 0)**. Either violation increments
`orientation_error_count` and clears `is_oriented`/`is_valid`, so `ComputeVolume`
refuses rather than returning `V_outer + V_inner` for a mis-wound cavity. A
bbox-scale relative guard skips genuinely degenerate (near-planar) shells before
the sign test. The `test/cpp/test_inner_shell.cpp` same-wound case now asserts
rejection.

Instead of bbox/point-in-solid containment (the original sketch), the exterior is
taken as shell 0 (CityJSON writes the outer shell first) and the magnitude test
catches a mislabelled larger "interior" shell. Scope is **relative** opposition
only: the exterior's absolute outward orientation and true point-in-polyhedron
containment remain out of scope (a future absolute-orientation check would
complete full §9.3 conformance without touching this one).
