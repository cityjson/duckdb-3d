# Future Work

Focused, actionable notes on deferred capabilities. This complements the short roadmap in
[DESIGN_DOC.md §11](./DESIGN_DOC.md#11-roadmap): that section lists *what* is deferred, while
this file records the **larger design decisions** behind these areas and what "done" would
require. The implemented surface is catalogued in [FUNCTIONS.md](./FUNCTIONS.md).

Each item follows the repository's TDD discipline: failing `test/cpp/` + `test/sql/` first,
then implementation, then a design-doc update in the same change.

---

## 1. Move CityJSON-aware Interpretation Out Of `duckdb-3d`

### The concern (separation of concerns)

`duckdb-3d` is meant to be a **CityJSON-agnostic** 3D kernel (DESIGN_DOC §1, §2;
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
interoperability contract (DESIGN_DOC §7) should be restated in these neutral terms, and
`metadata_parser.cpp` reduced to the neutral schema.

This also makes multi-solid interior-shell grouping an upstream concern producing neutral
grouping counts, rather than a CityJSON special case inside the kernel.

---

## 2. Coordinate Reference System Support (SRID)

### The constraint

Coordinates are raw `DOUBLE` XYZ with **no stored SRID**, so both CRSs must be given on every
`ST_3DTransform` call, and all measurement math is Cartesian in the input units. Callers
reproject into a suitable metric CRS before measuring. See
[FUNCTIONS.md](./FUNCTIONS.md#st_3dtransform--crs-reprojection) for the current semantics.

### Open work

1. **Vertical datum / 3D reprojection.** Currently Z is passed through untouched. Ellipsoidal↔
   orthometric height and geoid models are out of scope; genuinely hard and rarely needed for
   the city-model workflows here. Revisit only on demand.
2. **Stored SRID + `ST_SRID` / `ST_SetSRID`.** Add an SRID field to the `D3DS`/`D3DG` payload
   headers (a versioned change under DESIGN_DOC §5.1). This would enable the single-argument
   `ST_3DTransform(geom, target_srid)` form (reading the stored source SRID, like PostGIS) and
   cross-CRS mismatch detection (e.g. refuse `ST_3DDistance` across differing SRIDs). Lowest
   cost, high safety value — the natural next step.
3. **`proj.db` distribution bundling.** Reprojection depends on PROJ's datum database at
   runtime. Locally it resolves via the Homebrew/vcpkg install path; bundling it into a single
   distributable `.duckdb_extension` (as `duckdb_spatial` does, via
   `proj_context_set_search_paths`) is still outstanding and is the main gap before shipping
   CRS support in a released binary.
