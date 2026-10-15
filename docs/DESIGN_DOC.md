# duckdb-3d — Architecture & Design

**What this document is.** The architectural source of truth for the `duckdb-3d` extension:
the design philosophy, the type model, the layering, and the invariants every change must
preserve. It explains **why** the extension is shaped the way it is.

**What this document is not.** It is deliberately **not** a function catalogue or an
algorithm listing. Per-function signatures, return types, and behaviour live in
[FUNCTIONS.md](./FUNCTIONS.md), which is verified against a real build — keeping them out of
here is what stops this document drifting from the code.

| I want… | Read |
| --- | --- |
| Signatures, return types, examples | [FUNCTIONS.md](./FUNCTIONS.md) |
| A narrative walkthrough on real data | [EXAMPLE.md](./EXAMPLE.md) |
| Composing with the `cityjson` extension | [CITYJSON_INTEROP.md](./CITYJSON_INTEROP.md) |
| Deferred design decisions | [FUTURE_WORK.md](./FUTURE_WORK.md) |
| Build & test mechanics | [README.md](./README.md) |

**Audience.** Engineers and coding agents implementing, extending, or reviewing the
extension.

---

## 1. Purpose

`duckdb-3d` makes the polyhedral solids in 3D city models first-class, queryable values in
DuckDB.

DuckDB can store binary geometry and the `spatial` extension provides a capable 2D /
simple-features surface, but neither answers the questions 3D city-model work actually asks:

- What is the enclosed volume of this building?
- Is this solid closed and manifold — is it a solid at all, or just a bag of faces?
- What is its footprint area and height?
- Which buildings are within 5 m of each other in 3D?

The extension fills that gap with solid-aware types and a curated `ST_3D*` function family.

**It is not a CityJSON extension.** CityJSON is an important upstream *producer*, but the
public API is format-neutral: any source that can emit WKB polyhedral surfaces, plus optional
shell-grouping metadata, is a first-class citizen. CityJSON knowledge is confined to the
documented interoperability layer (§7), never the kernel.

---

## 2. Design philosophy

Five commitments shape almost every decision in the codebase.

### 2.1 Validate, never repair

The extension reports what is wrong with a geometry; it does not fix it. An open shell stays
open, inconsistent winding stays inconsistent, and `ST_3DVolume` **raises** rather than
returning a plausible-looking number derived from broken topology.

This is a research-data stance: silent repair destroys the evidence that a reconstruction
pipeline produced bad output. Callers who want tolerance opt into it explicitly, via the
`TRY` constructors and `ST_3DValidationReport`.

The only normalization allowed at import is lossless bookkeeping — dropping duplicate
consecutive ring vertices, dropping WKB's closing-vertex repetition, and canonical ordering
of internal offsets. Closing shells, merging dangling edges, flipping winding, or patching
non-manifold topology are all forbidden.

### 2.2 Topology is the source of truth; triangulation is a cache

The canonical model stores original **polygonal** faces and their shell grouping. Triangles
are derived, cached in the payload for fast area and volume math, and may be recomputed at
any time. Nothing may treat the triangulation as authoritative, because doing so would
quietly discard the face structure that WKB export and semantic-surface interoperability
depend on.

A face is triangulated as a whole: its exterior ring and its holes go to one ear-clipping
pass (the vendored `mapbox/earcut.hpp`), which bridges each hole into the exterior. Holes are
never triangulated as polygons of their own — that would lay their triangles over the
exterior's, and only an opposite winding would cancel them out of the volume sum. Every
triangle is wound like the exterior ring, so a triangle's orientation follows its face's.

The rings are projected into the face's own plane — an orthonormal frame perpendicular to its
Newell normal — rather than onto the coordinate plane the normal leans on most. On a
near-vertical sliver whose normal sits between two axes, dropping an axis can put two
distinct vertices on one 2D point, and one of them would vanish from the triangulation.
Coordinates are referenced to the exterior ring's first vertex, for the same conditioning
reason volume is (§8.2), one power lower: every test the triangulation makes is a signed area
whose products scale as `|position|²` against an answer of `|extent|²`.

earcut drops a vertex that is collinear with its neighbours in the plane. On a face that is
only nearly planar such a vertex can still sit off the face in 3D while its neighbouring face
uses it, and skipping it would open a sliver gap in the triangulated surface. Every dropped
vertex is therefore put back by splitting the one triangle on the boundary edge it lies along,
so the triangles use every ring vertex.

**A triangulation is all or nothing.** The triangles are accepted only if they tile the face —
all wound one way, their areas summing to the exterior's area less the holes'. A ring that
crosses itself, or a hole that leaves its exterior, has no such tiling; the face then keeps
**no** triangles rather than a partial set, and validation counts it degenerate (§8.1), which
`ST_3DVolume` and `ST_3DSurfaceArea` refuse. The same holds when earcut's fallback — splitting
a polygon it finds no ear on and recursing into both halves — would nest more than 32 deep:
the vendored copy bounds that recursion, because a self-overlapping ring from SQL could
otherwise nest once per vertex and exhaust a worker thread's stack. A partial triangulation would be worse than
none: volume sums triangles while validation reads rings, so a silently incomplete face would
give a wrong `ST_3DVolume` with every validity flag still green. Pinned by
`test/cpp/test_triangulation.cpp`, on real 3DBAG and railway faces.

### 2.3 Fail clearly at the boundary

Unsupported geometry classes are rejected with a descriptive error at import, not silently
coerced into something the kernel can chew on. Errors name the function and the reason.

### 2.4 Self-contained kernel

The current function set runs on a pure C++ geometry kernel with no CGAL or SFCGAL
dependency. This keeps the extension small, portable, and cheap to build. Orientation tests
that must be exact — the 3D convex hull's, `ST_3DIntersects` and the point-in-solid test of
the solid overloads — use Shewchuk's adaptive predicates, vendored
under `third_party/predicates/`; they are exact on double input and cost a plain
floating-point evaluation unless the answer is close to zero. Everything that needs exact
constructions rather than exact signs — 3D booleans, skeletons, medial axes — is
deliberately **out of scope** rather than approximated. PROJ is the one external library,
confined to CRS reprojection; face triangulation uses the header-only `mapbox/earcut.hpp`,
vendored under `third_party/` with its split recursion bounded.

### 2.5 Coexist with `spatial`, don't compete

`spatial` owns the generic 2D vocabulary; `three_d` owns the 3D-solid vocabulary. See §3.2.

---

## 3. Naming

### 3.1 `three_d`, not `3d`

- Repository / product name: **`duckdb-3d`**
- Extension target, entrypoint, and load name: **`three_d`** (`LOAD three_d;`)
- SQL function family: **`ST_3D*`**

DuckDB's C++ extension entry macro expands the extension name into a C++ symbol, which must
be a valid identifier — `3d` is not. `LOAD 3d` would also need quoting. If a packaging alias
to `3d` becomes possible later it can be added without touching function names.

### 3.2 Why `ST_3D*`

`spatial` registers `ST_Area`, `ST_Scale`, `ST_X`, `ST_ZMax`, `ST_AsText`, … on its
`GEOMETRY` type, and registers them `ERROR_ON_CONFLICT`. If `three_d` claimed those names,
loading both extensions in one session would abort — **in either order**, since whichever
registers second throws.

Prefixing every colliding name with `ST_3D*` (PostGIS's own convention for 3D variants)
removes the conflict entirely. The two extensions load together in any order, and because
`SOLID_3D` / `GEOM_3D` are distinct type aliases, overloads never cross-bind.

Names that `spatial` does **not** define stay un-prefixed: `ST_Force3D`, `ST_MakeSolid`,
`ST_NDims`, `ST_CoordDim`, `ST_IsPlanar`, `ST_Geom3DFromWKB`.

One rename is non-obvious: `ST_3DArea` is *surface* area, so the 2D footprint is
`ST_3DFootprintArea`.

Full PostGIS parity is an explicit non-goal. This is a curated subset chosen for 3D
city-model workflows.

---

## 4. Type model

Two SQL-visible types, both registered as named aliases over `BLOB` carrying a versioned,
opaque payload that only this extension interprets.

| Type | Holds | Payload magic |
| --- | --- | --- |
| `SOLID_3D` | Closed polyhedral solids: solid → shell → face → ring topology, plus cached validation flags. | `D3DS` |
| `GEOM_3D` | General 3D geometry: points, lines, polygons, their multi- forms, and polyhedral surfaces. | `D3DG` |

**Why an alias over `BLOB` rather than a new storage primitive?** DuckDB's named-type
registration is well-supported and needs no new physical type; a versioned opaque payload
gives full control over topology preservation and forward compatibility; and the value stays
efficient for vectorized execution and cached materialization.

**Consequences of the typed return.** Constructors return the alias, not plain `BLOB`, and
functions take only the alias: none has a `BLOB` overload. A payload carries caches —
the triangulation, the bounding box, the validation verdict — that decide what functions
return, so a function that accepted any `BLOB` would trust caches nothing has checked. A
payload read back from Parquet, or written by another tool, arrives as `BLOB` and becomes a
`SOLID_3D` or `GEOM_3D` only through an explicit cast (`::SOLID_3D`, `::GEOM_3D`), which
re-derives every cache from the topology and vertices and refuses bytes that are not a
payload of that type. The rebuild costs a triangulation and, for a solid, a validation, so
the casts are explicit: a query pays for it once when it materialises the cast. The reverse
casts (`…::BLOB`) reinterpret the payload bytes; use `ST_3DAsWKB` / `ST_3DAsBinary` for
interchange.

**Why two types.** Solid-only operations (volume, closedness, shell counts) require topology
that a point or a linestring simply does not have. Rather than making every such function
fail at runtime on the wrong geometry class, the type system rejects it at bind time.
Class-generic operations (distance, serialization, most accessors) take `GEOM_3D`.

**Null semantics.** Standard DuckDB propagation: a `NULL` required argument gives `NULL`.
`TRY` constructors return `NULL` instead of raising on unsupported input; non-`TRY`
constructors raise descriptive errors. The deliberate exceptions, all optional inputs, are documented in
[FUNCTIONS.md](./FUNCTIONS.md#conventions).

---

## 5. Canonical solid model

The in-memory model preserves enough information for validation, measurement, counts and
bounds, WKB export, and a future backend swap without SQL changes.

**Core topology** — a unique vertex array in transformed XYZ, plus nested offset arrays:
solid → shell, shell → face, face → ring, ring → vertex indices. This keeps original
polygonal faces intact rather than flattening to triangles (§2.2).

**Derived caches** — per-face triangulation ranges and triangle indices, the 3D bounding
box, and the topology validity flags. All are recomputable from the core topology, and the
`BLOB` casts recompute them (§4).

**Coordinates** are raw `DOUBLE` XYZ. The payload has a slot for a CRS string, which every
constructor leaves empty; upstream producers apply any source transform before or during
WKB construction, and `ST_3DTransform` reprojects explicitly, with both CRSs given per
call. All measurement math is Cartesian in the input units.

### 5.1 Binary payload format

`SOLID_3D` uses a versioned binary layout (version 2): a header (magic `D3DS`, `u16`
major / `u16` minor version, flags, counts and the bounding box), the CRS as a `u32` byte
length and its bytes, the offset arrays, the data arrays (vertices, ring indices,
triangles), and a validation-cache trailer — seven `u32` counters (open edges,
non-manifold edges, degenerate faces, orientation errors, non-manifold vertices,
disconnected shells, self-intersections) and a flags word (closed, manifold, oriented,
valid). `GEOM_3D` uses the parallel `D3DG` layout (version 2): magic, version, geometry
type code, bounding box and vertex count, the CRS, the vertices, the ring and part
offsets, and the triangulation cache of its polygons (per-polygon triangle offsets and the
triangle indices).

A `GEOM_3D` polygon is triangulated once, when the value is built, by the same face
triangulator as a solid's faces (§2.2), so distance never re-triangulates per pair. A
polygon that admits no tiling — a ring that crosses itself, a hole outside its exterior —
is refused there, at the constructor (`TRY` gives `NULL`), rather than inside a join. A
polygon with no area keeps no triangles; its boundary is all there is of it.

Both keep the bounding box and counts in the **front header**, so introspection functions
(`ST_3DBounds`, `ST_3DNumFaces`, `ST_3DValidationReport`, …) are O(1) reads that never
materialise the geometry — this is what makes them cheap enough to use as query filters.

**Compatibility rules.** The payload is an internal format, but it is persisted in user
tables and Parquet files, so it is versioned:

- A **minor** bump may only add trailing, optional data that older readers can ignore.
- Any change to existing field meaning or layout requires a **major** bump.
- A reader must reject a payload whose major version it does not know. Version 1 payloads
  are refused with a message to rebuild the value from its WKB.
- Changing the format requires updating this section in the same change.

**Stored payloads are untrusted.** Every read checks magic, version and that every offset
and index stays in range, so malformed bytes raise rather than read out of bounds. What a
read cannot check is that the caches agree with the geometry; that is why a `BLOB` reaches
the functions only through the rebuilding casts (§4). The trust boundary is DuckDB's own
storage: a `SOLID_3D` or `GEOM_3D` column in a `.duckdb` database keeps its type when the
file is reopened, and its caches are trusted as they were written, as any column's values
are. Anything that arrives as a `BLOB` — Parquet, CSV, another tool's table — is rebuilt.

---

## 6. Layering

Four layers, strictly ordered. Each may depend only on those below it.

```
┌─ Registration ── src/three_d_extension.cpp
│    registers SOLID_3D / GEOM_3D and every scalar function
├─ SQL functions ── src/functions/
│    bind arguments, decode payloads, drive vectorized execution
├─ Kernel ──────── src/kernel/
│    WKB parsing, model construction, triangulation, topology
│    analysis, measurement, serialization, CRS transforms
└─ Errors ──────── stable codes, user-facing text, TRY null-mapping
```

**The load-bearing rule: the kernel knows nothing about DuckDB, and nothing about CityJSON.**
It operates on plain C++ geometry structures. This is what keeps the geometry logic unit-
testable in `test/cpp/` without a database, and what would let a CGAL/SFCGAL backend be
swapped in behind the same SQL surface.

Correspondingly, the SQL layer holds no geometry math — it only marshals values in and out
of the kernel.

**One boundary is knowingly imperfect.** `src/kernel/metadata_parser.cpp` still understands
CityJSON-flavoured `geometry_properties` keys. Moving that interpretation upstream into
`duckdb-cityjson`, behind a format-neutral shell-grouping descriptor, is tracked in
[FUTURE_WORK.md §2](./FUTURE_WORK.md).

---

## 7. Interoperability contract

Integration with [`duckdb-cityjson`](https://github.com/cityjson/duckdb-cityjson) is **SQL
composition, not a runtime dependency**. Neither extension links the other.

```
CityJSON ──cityjson──▶ (geometry BLOB, geometry_properties) ──three_d──▶ SOLID_3D / GEOM_3D
```

The contract is exactly two things:

1. The producer supplies **WKB** — `PolyhedralSurface Z`, or `GeometryCollection Z` of those —
   as a `BLOB`, or as DuckDB's `GEOMETRY` when a Parquet-annotated column arrives that way
   (converted back to WKB at the boundary).
2. The producer *may* supply a **`geometry_properties` sidecar**, as JSON text or as a
   CityParquet `geometry_properties_lod*` `STRUCT`. Both forms are accepted directly.

**Why the sidecar exists.** WKB `PolyhedralSurface Z` has no vocabulary for shell grouping.
Without metadata, a polyhedral surface imports as one solid with one shell — which is
correct for simple buildings but loses the exterior/interior distinction for solids with
cavities. The sidecar's `shells` key restores that grouping, which in turn enables truthful
`ST_3DNumShells` and the interior-shell winding check of §8.3.

This keeps `duckdb-3d` ignorant of CityJSON files, LoD selection, and semantic surfaces —
all of which stay upstream.

---

## 8. Validation & measurement semantics

The definitions below are contract, not implementation notes — they are what the numbers
*mean*.

### 8.1 The checks

- **Closed** — every undirected edge is referenced exactly twice, with opposing local
  orientation.
- **Manifold** — no undirected edge belongs to more than two incident faces, and no vertex
  is pinched: within one edge-connected part of a shell, the faces around a vertex form a
  single fan (val3dity 303).
- **Oriented** — face winding is consistent within each shell, *and* every interior shell
  is a cavity: wound opposite the exterior shell, smaller than it, inside it, and not inside
  another interior shell (val3dity 403 and 401). Containment is the winding number of one
  shell's cached triangles about the other's vertices.
- **Connected** — every shell's faces are connected through shared edges. Two boxes that
  touch at a vertex or not at all are two parts, not one shell (val3dity 305).
- **Free of self-intersection** — no two faces of a solid cross each other, within a shell or
  across its shells (val3dity 306, and 401 for shells that cross).

A face is **degenerate** if fewer than 3 distinct vertices survive normalization,
triangulation fails (the face cannot be tiled, §2.2), or its area is zero within tolerance.

Validation runs at import and its result is cached in the payload, so the predicates are
reads rather than recomputation. `is_valid` requires every check and zero degenerate faces.

`ST_3DVolume` requires all of them except connectivity — a solid whose shell falls apart
into closed parts still has a well-defined volume, their sum; `ST_3DSurfaceArea` requires
only the absence of degenerate faces. Both **raise** when unmet (§2.1).

**Self-intersection is decided on the triangulation.** Each face is represented by its
cached triangles, and a pair of triangles from different faces counts when:

- each reaches further than the tolerance to *both* sides of the other's **face** plane,
- the segments where each meets the other's **triangle** plane overlap by more than the
  tolerance, and
- that overlap lies further than the tolerance from the boundary of at least one of the
  two faces.

Three decisions, three numbers. Depth is measured against the face's Newell plane, not the
triangle's, because earcut tiles faces into slivers whose own planes are ill-defined: a
sliver's plane can be tilted far enough that a neighbour seems to pierce it by metres. Where
the triangles cross is measured on the triangles themselves, whose shared vertices lie
exactly on both planes. The boundary condition removes folds of a junction: two roof faces
whose ridges cross each other by a centimetre, along slivers at the ridge, cross only
within the tolerance of both faces' edges, and a different tiling of the same rings need
not cross at all. A thin face piercing a wall far from the wall's edges still counts,
however thin it is. Two triangles are coplanar when one lies on the other's face plane as
closely as the faces' own vertices do — within the sum of the two faces' deviations, plus
the rounding term — and count when their overlap in that plane is wider (twice the area
over the perimeter) than the tolerance. Measuring coplanarity against the deviations rather
than the rounding term alone is what catches a triangle folded back onto a warped
neighbour: a face that repeats part of another face's ring, wound the other way, uses every
edge twice in opposite directions and is visible only as an overlap. Faces closer than the tolerance that do not cross are not
counted. Candidate pairs come from a sweep over triangle bounding boxes along the solid's
longest axis, and a pair of faces is counted once however many of their triangles cross.

The tolerance is `2·(dev_a + dev_b)` plus `1e-5` times the solid's bounding-box diagonal,
where `dev` is a face's largest distance from its Newell plane through its centroid. The
second term is the scale of coordinate rounding: city models publish millimetre
coordinates for objects tens of metres across, and two nearly planar faces meeting at a
vertex can cross by half a millimetre for no other reason (a 3DBAG building in the frozen
golden set does). A non-planar face has no single surface: every triangulation of it lies within
`dev` of that plane, so two triangulations of it can disagree by `2·dev`, and an apparent
crossing smaller than that says more about which triangulation was chosen than about the
solid. It follows that the triangulation cache is part of a validity verdict, which is one
more reason the `BLOB` casts rebuild it (§4) rather than trust it. val3dity is exact on its
own triangulation after snapping vertices to 1 mm; it and this rule agree on every 3DBAG
Delft solid apart from planarity and snapping differences, and on Helsinki every
self-intersection val3dity finds that this rule does not is within the faces' deviation
(§8.4).

**Tolerances** are named constants in `src/kernel/core_types.hpp`, and every one that
decides validity is relative, so a verdict survives rigid motions and uniform scaling:
measured on the 1116 Delft LoD2.2 solids, rotating, translating, or scaling them by 1000 or
by 0.001 changes no verdict. Scaling the axes unequally changes angles and planarity, so it
may change a verdict on a face whose features are already at tolerance scale (10 of the
1116 under a tenfold vertical stretch, each a fold or crossing at tolerance scale before the
stretch).

A face's area counts as zero when its Newell area vector is smaller than `kEpsRelative`
times the square of the ring's extent. The vector is computed about the ring's own first
vertex (`NewellRingAreaVector`): it is translation-invariant, and on absolute projected
coordinates a face of exactly zero area would otherwise return noise of order
`n·eps·|position|·|extent|` — 1e-11 to 1e-10 at RD New easting/northing — and the verdict
would depend on where the building sits. Because `degenerate_face_count` gates `ST_3DVolume`
and `ST_3DSurfaceArea`, so would a hard error. The face triangulator applies the same
relative test before tiling.

### 8.2 Why volume works the way it does

Volume sums signed tetrahedral contributions over oriented triangles, taking the absolute
value **once per solid, after all shells have been summed**:

```
for each solid:
    solid_volume  = Σ over all shells, faces, triangles of signed tetra volume
    total_volume += abs(solid_volume)
```

Two consequences follow, and both are deliberate:

**Cavities subtract automatically.** By the orientation contract, an interior shell is wound
opposite the exterior, so it contributes negative signed volume. The net is
`V_outer − V_inner` with no "this shell is a hole" branch anywhere in the code. Inner versus
outer is recognised by *orientation*, not by a stored flag — the same way PostGIS/SFCGAL does
it, and necessarily so, because WKB carries no shell-role token.

**`abs` must be per solid, not per shell.** Global winding handedness is ambiguous — the
orientation check enforces *consistency*, not a fixed handedness. Taking `abs` once, after
the interior and exterior terms have already cancelled, collapses that ambiguity while
preserving the cavity subtraction. Taking it per shell would make cavities *add*, which would
be wrong.

**The reference point must be per shell, not per model.** The signed tetrahedra are taken
about a local reference point — the shell's first triangulated vertex — rather than about the
absolute coordinate origin. This is a no-op in exact arithmetic (a closed shell's signed
volume is translation-invariant), but it is what makes the sum survive doubles: on absolute
coordinates the triple product `a·(b×c)` scales as `|position|³` while the answer scales as
`|extent|³`, so at projected-CRS magnitudes almost every significant digit cancels. A
building in EPSG:28992 (easting/northing ~10⁵) loses roughly nine of the ~16 available
digits, and by 10⁸ the result is noise.

Note the *granularity*, which is the part that is easy to get wrong. The reference point must
sit **on the shell being integrated**, not merely somewhere inside the model. A single
model-wide point (say the model bounding-box midpoint) is close to its shells only when the
model is compact; for a `MultiSolid`/`CompositeSolid` whose parts are spatially separated it
is far from *every* part, the relative coordinates scale with half the part separation
instead of each part's own extent, and the cancellation returns in full. Measured on two unit
cubes separated diagonally and rotated 0.7 rad about X — true volume 16 — a model-wide point
returned 16.0027 at a separation of 10⁵, 14.456 at 10⁶ and 2353.1 at 10⁷, with
`is_valid` still `true` throughout. Per shell, the same cases land within 5e-11, 5e-10 and
7e-9 respectively.

Because each shell is separately closed (`ComputeVolume` refuses otherwise, and refuses a
self-intersecting solid, whose signed sum counts the overlap twice or not at all), giving each its
own reference point is exact and does not disturb the cavity subtraction above: shells still
accumulate **signed** into the per-solid total before `abs` is applied. `validation.cpp`'s
interior-shell winding check uses the same helper (`ShellLocalOrigin`, `geometry_math.hpp`),
so the sign it tests and the magnitude volume reports come from bit-identical arithmetic.

Do not "simplify" the reference-point subtraction away, and do not hoist it to model scope.
Both are pinned by `test/cpp/test_measurements.cpp` and `test/sql/st_3d_multisolid.test`.

### 8.3 What shell grouping actually buys

A correctly-wound cavity yields the correct volume **even without** shell metadata, because
the signed sum is driven by winding, not by grouping. What the `shells` sidecar adds is:

- truthful introspection — `ST_3DNumShells` reports 2 rather than 1, and the one shell is
  not reported as disconnected, which without the sidecar it is (its exterior and cavity
  faces share no edge), so `is_valid` is false; and
- the shell partition needed to *enforce* the interior-opposite-exterior invariant. A
  same-wound cavity would otherwise silently **add** its volume; with the partition available
  the check clears `is_oriented` / `is_valid`, and `ST_3DVolume` refuses.

The check is relative: the absolute outward orientation of the exterior shell is out of
scope. Containment is not — an interior shell outside the exterior, or inside another
cavity, would subtract volume that is not there, so it counts as an orientation error.

**Surface area and footprint are not shell-aware.** They sum over all faces, so cavity walls
contribute to reported surface area. Only volume distinguishes shell roles.

### 8.4 Independent verification

Measurement math is cross-checked two ways, neither of which is a build or runtime
dependency:

- **PostGIS + SFCGAL as a differential oracle.** An offline harness feeds *identical WKB
  bytes* to both engines and freezes SFCGAL's answers into `test/data/postgis_oracle/`. CI
  replays the frozen values with no PostGIS present. Feeding the same bytes to both isolates
  the math from ingestion differences. Where SFCGAL rejects real reconstructed roofs for
  non-planarity — the extension measures them by triangulation instead — the oracle of record
  is 3DBAG's published attributes, not SFCGAL. For *invalid* geometry PostGIS is the wrong
  oracle entirely, since it repairs or rejects; `ST_3DValidationReport` is authoritative
  there.
- **Ground truth from 3DBAG.** Volumes and footprint areas are compared against 3DBAG's own
  published per-building figures. Current agreement is a median volume error of **0.017 %**
  across ~1100 Delft buildings.
- **val3dity for validity.** val3dity, built from source and run on the 3DBAG Delft tile with
  its defaults (1 mm snapping, 1 cm planarity), agrees with `is_valid` on 1109 of 1116 LoD2.2
  solids and all 1116 LoD1.2 solids. The seven LoD2.2 differences are tolerances, not checks:
  one roof val3dity finds non-planar (duckdb-3d does not test planarity), five solids whose
  faces collapse to two vertices under 1 mm snapping (duckdb-3d does not snap), and one with
  a repeated consecutive vertex, which import drops as lossless bookkeeping (§2.1). None
  comes from connectivity, non-manifold vertices or self-intersection. val3dity reports no
  self-intersection on that tile, so the detector's positives are tested on Helsinki
  (`Helsinki_tex`, 77 249 LoD 2 solids) and by constructed solids in
  `test/cpp/test_validation.cpp`. On Helsinki the two agree on 76 839 solids (99.5 %). Of the
  410 differences, 355 are planarity, 23 are vertices exactly 1 mm apart that val3dity
  snaps together, 9 are faces duckdb-3d finds degenerate, and 1 is a polygon interior
  val3dity finds disconnected; the rest concern self-intersection. val3dity rejects 21 solids
  for a self-intersection (306) that duckdb-3d accepts: 18 cross by less than `2·(dev_a +
  dev_b)`, one by less than the `1e-5` term beyond that, and two do not cross at all on
  duckdb-3d's triangulation. duckdb-3d rejects one solid val3dity accepts: a
  triangle folded back onto its 1.5 mm-warped wall, overlapping it over a strip 2.1 cm wide.
  The solids val3dity calls non-manifold (303) only because a face folds back onto a
  neighbour are self-intersections here, and all are caught.
- **Live comparison on real data.** `scripts/oracle/live_diff.py` runs the distance family on
  five published datasets (3DBAG Delft, the TU Delft railway LoD3 model, Rotterdam
  Delfshaven, Den Haag, Vienna) in duckdb-3d and PostGIS and reports every disagreement. It
  is a development tool, not a CI step. Each row is compared against the reference that is
  right for it: PostGIS for planar faces without holes; an independent computation in the
  script for faces with holes, where PostGIS's 3D point-in-polygon test ignores interior
  rings; and the same computation, within twice the faces' deviation from planar, for
  non-planar faces, which PostGIS measures against a plane of its own fitting that can lie
  further from the vertices than the deviation.

### 8.5 Distance is an exact minimum over element pairs

The distance family decomposes each geometry into points, segments and triangles — a
polygon contributes its cached triangles, holes excluded (§5.1), and a polygon with no
area its boundary edges — and answers from the element pairs: `ST_3DDistance` is their
minimum, `ST_3DClosestPoint` / `ST_3DShortestLine` report the first pair in sweep order
that attains it (the point on the first geometry, then the point on the second),
`ST_3DDWithin` stops at the first pair within the threshold, and `ST_3DIntersects` at the
first pair that shares a point.

A point's nearest point on a triangle comes from Ericson's region tests, except in the
interior, where it is the projection onto the triangle's plane: Ericson's barycentric
weights divide by a sliver's tiny squared area, and a face triangulation has slivers
wherever a ring has near-collinear vertices.

Element pairs whose bounding boxes are farther apart than the bound that matters — the best
distance so far, or the threshold — are skipped. The box gap is a lower bound on the pair's
distance, so a skipped pair could never lower the minimum, win the closest-pair sweep, or fall
within the threshold: the answer is the one the full sweep gives. Two details keep it so. The
bound carries a slack of `kEpsRelative` times the coordinates' magnitude, because an element
distance is computed in doubles and could round onto the bound while its box gap sits just
above it. And the sweep is seeded with the exact distance of one nearby pair, found in linear
time, which is only ever used as a bound: the result is still taken from the sweep, so its
value and its tie-breaking do not depend on the seed. Pinned against an unpruned sweep by
`test/cpp/test_geom_distance.cpp`.

On planar faces without holes the result is PostGIS's: on 3DBAG Delft object pairs, face
pairs and points around faces, `ST_3DDistance` agrees with PostGIS's to 1.3e-10 m and the
predicates agree on every row (§8.4).

**`ST_3DIntersects` is exact.** Whether two elements share a point is decided by the signs of
Shewchuk's adaptive `orient2d` / `orient3d` (§2.4) and comparisons of input coordinates,
never by a distance: a point is on a segment when the three are collinear and it lies in the
segment's box; two segments meet when they are coplanar and cross (or touch) in a projection
that keeps their plane one-to-one, or, collinear, when an endpoint of one lies on the other;
a point is in a triangle when it is coplanar and on the inner side of every edge in such a
projection; a segment that crosses a triangle's plane meets the triangle when the line through
it passes on the same side of (or along) each edge, and a coplanar one when an endpoint is
inside or it crosses an edge; and two
triangles meet exactly when an edge of one meets the other (the ends of their common segment,
or the boundary of their common polygon, lie on edges). A triangle without area is its edges.
Pruning on element boxes is exact as well, since a box is the minimum and maximum of input
coordinates. So touching counts at any coordinate magnitude and a gap of one unit in the last
place is a gap: the predicate is PostGIS's exact zero, not a tolerance. `ST_3DDWithin(a, b, 0)`
stays a distance in doubles.

### 8.6 Solids are volumes

The `SOLID_3D` overloads of `ST_3DDistance`, `ST_3DDWithin` and `ST_3DIntersects` measure the
volume the shells bound, cavities excluded. The answer is the surface's — the solid's cached
triangles taken as elements — except that it is 0 (true) when a part of the other geometry
lies inside the volume. When the two surfaces do not meet, each connected part of the other
geometry lies wholly inside the volume or wholly outside it, so one point per part decides:
every point of a point set, the first vertex of each line and of each polygon, and one vertex
of each shell of a solid, since a valid solid's shells are edge-connected (§8.1). Two solids
are tested both ways.

Containment is an exact ray test. The vertical ray up from the point crosses some of the
solid's triangles; each counts +1 when it faces up and −1 when it faces down, and the sum over
one solid's shells is its winding number about the point: ±1 in the material, 0 outside and
in a cavity, because an interior shell is wound opposite the exterior (§8.1), whichever way
the exterior is wound. Whether the ray passes through a triangle is `orient2d` in the XY
plane, with the point perturbed symbolically by (ε, ε²) when it lies exactly on an edge's
line: the tie-break depends only on the directed edge and flips with it, so a ray through a
vertex or along an edge counts each crossing once. A vertical triangle has no area under the
ray and is skipped. Whether the triangle is above the point is `orient3d`. The test is exact
for every point off the surface, and the callers settle the surface first: in
`ST_3DIntersects` that test is exact too, so the predicate is exact; in `ST_3DDistance` and
`ST_3DDWithin` a point within rounding of the surface already has a surface distance within
rounding of 0.

A volume exists only for a valid solid (`is_valid`, §8.1): a gap, a fold or a mis-wound shell
leaves the winding number without meaning. The overloads therefore raise for an invalid solid,
reading the cached verdict from the header, so the error does not depend on whether the pair
is near enough to need the body.

---

## 9. Development workflow

**Strict test-driven development is mandatory**, not aspirational.

1. Write a failing test.
2. Implement the smallest change that makes it pass.
3. Refactor while green.

Rules:

- No new public function is started until a failing test exists.
- Every bug fix begins with a regression test that fails before the fix.
- Refactors are behaviour-preserving and covered by existing tests.
- One behaviour per test change; no large speculative test bundles.

**Test placement.** Geometry logic — WKB parsing, model construction, payload round-trips,
validation, triangulation, area/volume math — goes in `test/cpp/`. SQL-surface behaviour —
binding, null handling, `TRY` semantics, result contracts, `cityjson` interop — goes in
`test/sql/`. When both apply, write the unit test first.

**Test fixtures.** The `ST_AsWKB*` helper functions are gated behind the
`THREE_D_TEST_FIXTURES` environment variable, which the `Makefile` exports. Running a built
binary directly without it silently skips every test file that requires it.

---

## 10. Contribution invariants

A change is complete only when:

- the SQL contract matches this document, or this document is updated intentionally in the
  same change;
- [FUNCTIONS.md](./FUNCTIONS.md) is updated for any change to a public signature or
  behaviour, with examples re-run against a real build;
- tests existed before the implementation changed (§9);
- no silent topology repair has been introduced (§2.1);
- the kernel gained no DuckDB or CityJSON knowledge (§6);
- any binary-format change is versioned explicitly (§5.1).

---

## 11. Roadmap

**Implemented.** Import/export, introspection, validation, and measurement on `SOLID_3D`;
the class-generic `GEOM_3D` accessor, distance, transform, construction, and serialization
surface; PROJ-backed `ST_3DTransform`. See the
[function index](./FUNCTIONS.md#function-index).

**Near-term.**

- Stored SRID in the payload header, enabling a one-argument `ST_3DTransform` and cross-CRS
  mismatch detection ([FUTURE_WORK.md §2](./FUTURE_WORK.md)).
- Bundling PROJ's `proj.db` into the distributable extension.
- Moving CityJSON-aware interpretation upstream ([FUTURE_WORK.md §1](./FUTURE_WORK.md)).

**Lower priority.** Remaining PostGIS-analogue accessors and serializers — `ST_HasM`, `ST_M`,
`ST_Zmflag`, `ST_3DLongestLine`, `ST_Affine`, `ST_FlipCoordinates`, `ST_SwapOrdinates`,
`ST_PointOnSurface`, `ST_Boundary`, `ST_AsX3D`, `ST_AsGML`, `ST_AsKML`.

**Deferred pending a backend decision.** Everything requiring robust exact arithmetic: 3D
booleans (union / difference / intersection), tessellation, straight
skeletons, medial axes, and topology-repair workflows. These are gated on whether to take on
a CGAL or SFCGAL dependency — a decision deliberately not yet made, per §2.4.
