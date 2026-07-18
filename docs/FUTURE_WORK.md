# Future Work

Focused, actionable notes on deferred capabilities. This complements the roadmap in
[DESIGN_DOC.md §14 and §16](./DESIGN_DOC.md#14-roadmap-beyond-the-current-surface): §14/§16
enumerate the per-function backlog, while this file records the **larger design decisions**
behind three deferred areas and what "done" would require.

Each item follows the repository's TDD discipline: failing `test/cpp/` + `test/sql/` first,
then implementation, then a design-doc update in the same change.

---

## 1. Composite / Multi-solid Support With Interior Shells

### Current behaviour

| Input | Result |
| --- | --- |
| `PolyhedralSurface Z`, plain WKB | one solid, **one shell** |
| `PolyhedralSurface Z` + `geometry_properties` (`shellCount`, `shellFaceCounts`) | one solid, **N shells** — interior shells recovered and measured (see [DESIGN_DOC §10.2.1](./DESIGN_DOC.md#1021-interior-inner-shell-handling--mechanism-and-rationale)) |
| `GeometryCollection Z` of `PolyhedralSurface Z`, plain WKB | collection of **single-shell solids** (no interior-shell recovery per member) |
| `type = MultiSolid` / `CompositeSolid`, or `solidCount > 1`, via metadata | **raises** — `"metadata-aware import for multi-solid geometries is unsupported in v1"` (`src/kernel/model_builder.cpp`) |

So the one gap is: **a multi/composite solid whose members have interior shells cannot be
imported with correct per-member shell grouping.** Volume of such an input is either rejected
or (via the plain path) computed as a set of solid outer shells, ignoring cavities.

### What "done" requires

The canonical model already supports the target shape — `solid_shell_offsets` can describe
several solids, each with several shells, and `ComputeVolume` already sums `abs(per-solid)`
across solids and sums signed shells within a solid. The missing pieces are **import
grouping** and a **metadata contract**:

1. **Metadata schema.** Extend the `geometry_properties` contract from a flat
   `shellFaceCounts` to a per-solid nesting, e.g. `solidFaceCounts: [[outer, inner, …], …]`
   (or equivalent `solidShellCounts` + flat `shellFaceCounts`). Parse it in
   `src/kernel/metadata_parser.cpp`.
2. **Builder.** Add a builder path that slices the flat face list into `solid → shell → face`
   using the nested counts, populating `solid_shell_offsets` and `shell_face_offsets` for
   `solid_count > 1`. Remove the `requests_multi_solid` guard once covered.
3. **Validation.** Confirm `ValidateSolidModel` treats each solid's shells independently
   (closedness/manifoldness are per-shell; orientation must hold the interior-opposite-
   exterior invariant per solid). Add tests for a two-solid input where one solid is hollow.
4. **Export.** `wkb_export.cpp` already emits multi-solid as `GeometryCollection Z` of
   `PolyhedralSurface Z`; verify round-trip once multi-shell members exist.

### Blocking dependency

This is gated by item 2 below — the multi-solid shell grouping must be *produced upstream*.
See §8.2.1 of the design doc: the current CityJSON extension's `geometry_properties` is not
yet rich enough to describe per-solid interior shells for `MultiSolid`/`CompositeSolid`.

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

## 3. Coordinate Reference System Support (`ST_Transform`, SRID)

### Status

**`ST_Transform` (horizontal / 2D) is implemented** — PROJ-backed reprojection of X/Y with Z
preserved unchanged (no vertical datum), matching PostGIS's default. Signatures:
`ST_Transform(geom, source_srid INTEGER, target_srid INTEGER)` and
`ST_Transform(geom, source_crs VARCHAR, target_crs VARCHAR)`, on `SOLID_3D` and `GEOM_3D`.
PROJ is confined to `src/kernel/crs_transform.cpp`. See the design at
[docs/superpowers/specs/2026-07-18-st-transform-design.md](./superpowers/specs/2026-07-18-st-transform-design.md).

The base limitation still holds: coordinates remain raw `DOUBLE` XYZ with **no stored SRID**,
so the CRS must be given explicitly on every call, and all *measurement* math (volume, area,
distance) is still Cartesian in the input units. `ST_Transform` lets callers reproject into a
suitable metric CRS before those measurements.

### Remaining work

1. **Vertical datum / 3D reprojection.** Currently Z is passed through untouched. Ellipsoidal↔
   orthometric height and geoid models are out of scope; genuinely hard and rarely needed for
   the city-model workflows here. Revisit only on demand.
2. **Stored SRID + `ST_SRID` / `ST_SetSRID`.** Add an SRID field to the `D3DS`/`D3DG` payload
   headers (a versioned change under DESIGN_DOC §7.5). This would enable the single-argument
   `ST_Transform(geom, target_srid)` form (reading the stored source SRID, like PostGIS) and
   cross-CRS mismatch detection (e.g. refuse `ST_3DDistance` across differing SRIDs). Lowest
   cost, high safety value — the natural next step.
3. **`proj.db` distribution bundling.** Reprojection depends on PROJ's datum database at
   runtime. Locally it resolves via the Homebrew/vcpkg install path; bundling it into a single
   distributable `.duckdb_extension` (as `duckdb_spatial` does, via
   `proj_context_set_search_paths`) is still outstanding and is the main gap before shipping
   CRS support in a released binary.
