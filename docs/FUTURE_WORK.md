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

Coordinates are raw `DOUBLE` XYZ, labelled with a CRS (DESIGN_DOC §5), and all measurement
math is Cartesian in the input units. Callers reproject into a suitable metric CRS before
measuring. See [FUNCTIONS.md](./FUNCTIONS.md#crs) for the current semantics.

### Open work

1. **Vertical datum / 3D reprojection.** Currently Z is passed through untouched. Ellipsoidal↔
   orthometric height and geoid models are out of scope; genuinely hard and rarely needed for
   the city-model workflows here. Revisit only on demand.
2. **`proj.db` distribution bundling.** Reprojection depends on PROJ's datum database at
   runtime. Locally it resolves via the Homebrew/vcpkg install path; bundling it into a single
   distributable `.duckdb_extension` (as `duckdb_spatial` does, via
   `proj_context_set_search_paths`) is still outstanding and is the main gap before shipping
   CRS support in a released binary.

## 3. Triangulation Time On Adversarial Rings

The vendored earcut bounds its split recursion, so a self-overlapping ring cannot exhaust the
stack, but `splitEarcut`'s diagonal search is still roughly cubic in the ring size before the
bound trips. Done means a cap on work (or ring size) that fails the face as degenerate, with a
test on a ring large enough to take seconds without it.

## 4. Planarity In Validation

`ST_3DValidationReport` does not test planarity; a non-planar face is measured through its
triangulation, and the self-intersection tolerance scales with each face's deviation from its
plane (DESIGN_DOC §8.1). val3dity rejects faces more than 1 cm from planar (its 203/204), and
that is the one class of verdict where the two differ on 3DBAG Delft. Done would mean a
planarity counter with a stated, unit-aware tolerance — or a decision that the triangulated
reading is the contract and planarity stays a separate `ST_IsPlanar` question.

## 5. A Zero-Copy Payload View

Measures on a materialised column decode each payload into a `SolidModel` first: 0.39 µs of
the 0.91 µs a solid that `ST_3DVolume` takes (DESIGN_DOC §5.2). A read-only view over the
payload bytes would skip the copy for the measures that walk the arrays once (volume, surface
area, footprint area). Done means a view type the measures accept alongside the model, a
query where decoding is shown to dominate, and the same results bit for bit.

## 6. Distance Between Large Geometries

The distance sweep (DESIGN_DOC §8.5) is fast on buildings and quadratic on large
triangulations whose triangles are long slivers, such as a 4 000-gon cap. A per-geometry tree
is slower on buildings and, like the sweep, visits every tied pair in the slow case. What would help the slow one is a bound tighter than a box (the
separation of two triangles' planes, say) or pruning pairs that can only tie the minimum,
which would change which tied pair `ST_3DClosestPoint` reports. Done means a stated tie rule
and a test on such a pair.
