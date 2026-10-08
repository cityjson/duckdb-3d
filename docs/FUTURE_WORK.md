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
repo `CLAUDE.md`: *"Keep CityJSON-specific assumptions out of the core kernel"*). The leak
is now narrow but real: `src/kernel/metadata_parser.cpp` parses the CityParquet spec §8
`geometry_properties` object — a CityGML/CityJSON-shaped document — and keeps a `type` field
whose vocabulary (`Solid`, `MultiSolid`, `CompositeSolid`) is CityJSON's. Grouping itself is
already driven entirely by the format-neutral `shells` face counts, and `type` is only
informational, so what remains is the *name and shape of the sidecar contract* rather than
any behavioural branch. Deciding how CityJSON geometries become DuckDB values is properly
the responsibility of the `duckdb-cityjson` extension.

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

## 3. Triangulation Outside The Solid Kernel

### The constraint

`SOLID_3D` faces are triangulated whole (holes bridged, every vertex kept, never partially;
DESIGN_DOC *Topology is the source of truth*). The distance family on `GEOM_3D` does not use
that triangulator: `geom_distance.cpp` fan-triangulates each face's exterior ring and ignores
its holes. A fan is only correct for a convex ring, so on a concave face some fan triangles lie
outside it, and a point there can measure distance 0 to the face; a point over a hole measures
distance to the hole's interior.

### Open work

1. **Route `GEOM_3D` surfaces through the face triangulator.** It works on a ring list and a
   vertex array, so the change is in `Decompose`; done means a concave-face and a holed-face
   distance case pinned against the PostGIS oracle.
2. **Bound triangulation time on adversarial rings.** The vendored earcut bounds its split
   recursion, so a self-overlapping ring cannot exhaust the stack, but `splitEarcut`'s diagonal
   search is still roughly cubic in the ring size before the bound trips. Done means a cap on
   work (or ring size) that fails the face as degenerate, with a test on a ring large enough to
   take seconds without it.
