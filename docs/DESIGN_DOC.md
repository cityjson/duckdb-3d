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

Ear-clipping runs on coordinates referenced to the ring's own first vertex, for the same
conditioning reason volume does (§8.2), one power lower: the handedness shoelace and the
convexity tests are signed areas whose products scale as `|position|²` against an answer of
`|extent|²`. Left absolute, the handedness sum collapsed for a 1 mm face at RD New northings
and for a 2 m face at ~10⁹; the convexity test then inverted, no ear was ever found, and the
face emitted **zero** triangles — a wrong `ST_3DVolume` with every validity flag still green,
because validation reads rings, not triangles. Pinned by `test/cpp/test_triangulation.cpp`.

### 2.3 Fail clearly at the boundary

Unsupported geometry classes are rejected with a descriptive error at import, not silently
coerced into something the kernel can chew on. Errors name the function and the reason.

### 2.4 Self-contained kernel

The current function set runs on a pure C++ geometry kernel with no CGAL or SFCGAL
dependency. This keeps the extension small, portable, and cheap to build. Everything that
would genuinely require a robust exact-arithmetic backend — 3D booleans, true 3D convex
hulls, skeletons, medial axes — is deliberately **out of scope** rather than approximated.
PROJ is the one external dependency, confined to CRS reprojection.

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

**Consequences of the typed return.** Constructors return the alias, not plain `BLOB`. So
generic `BLOB` consumers (`octet_length`, …) do not bind against constructor output, and
no `SOLID_3D` → `BLOB` cast is registered — `…::BLOB` raises rather than reinterpreting the
payload. Use `ST_3DAsWKB` when a real `BLOB` is wanted. For the same reason, a bare `NULL`
is ambiguous for any function carrying both a typed and a `BLOB` overload and must be
written `NULL::SOLID_3D`.

**Why two types.** Solid-only operations (volume, closedness, shell counts) require topology
that a point or a linestring simply does not have. Rather than making every such function
fail at runtime on the wrong geometry class, the type system rejects it at bind time.
Class-generic operations (distance, serialization, most accessors) take `GEOM_3D`.

**Null semantics.** Standard DuckDB propagation: a `NULL` required argument gives `NULL`.
`TRY` constructors return `NULL` instead of raising on unsupported input; non-`TRY`
constructors raise descriptive errors. The two deliberate exceptions are documented in
[FUNCTIONS.md](./FUNCTIONS.md#conventions).

---

## 5. Canonical solid model

The in-memory model preserves enough information for validation, measurement, counts and
bounds, WKB export, and a future backend swap without SQL changes.

**Core topology** — a unique vertex array in transformed XYZ, plus nested offset arrays:
solid → shell, shell → face, face → ring, ring → vertex indices. This keeps original
polygonal faces intact rather than flattening to triangles (§2.2).

**Derived caches** — per-face triangulation ranges and triangle indices, the 3D bounding
box, and the topology validity flags. All are recomputable from the core topology.

**Coordinates** are raw `DOUBLE` XYZ with **no stored CRS or SRID**. Upstream producers must
apply any source transform before or during WKB construction; `ST_3DTransform` reprojects
explicitly, with both CRSs given per call. All measurement math is Cartesian in the input
units.

### 5.1 Binary payload format

`SOLID_3D` uses a versioned binary layout: a header (magic `D3DS`, `u16` major / `u16` minor
version, flags, and counts), the offset arrays, the data arrays (vertices, ring indices,
triangles), and a validation-cache trailer. `GEOM_3D` uses the parallel `D3DG` layout with a
geometry type code in place of the solid validation trailer.

Both keep the bounding box and counts in the **front header**, so introspection functions
(`ST_3DBounds`, `ST_3DNumFaces`, `ST_3DValidationReport`, …) are O(1) reads that never
materialise the geometry — this is what makes them cheap enough to use as query filters.

**Compatibility rules.** The payload is an internal format, but it is persisted in user
tables and Parquet files, so it is versioned:

- A **minor** bump may only add trailing, optional data that older readers can ignore.
- Any change to existing field meaning or layout requires a **major** bump.
- A reader must reject a payload whose major version it does not know.
- Changing the format requires updating this section in the same change.

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

1. The producer supplies **WKB** as a `BLOB` — `PolyhedralSurface Z`, or `GeometryCollection Z`
   of those.
2. The producer *may* supply a **`geometry_properties` sidecar**, as JSON text or as a
   CityParquet `geometry_properties_lod*` `STRUCT`. Both forms are accepted directly.

**Why the sidecar exists.** WKB `PolyhedralSurface Z` has no vocabulary for shell grouping.
Without metadata, a polyhedral surface imports as one solid with one shell — which is
correct for simple buildings but loses the exterior/interior distinction for solids with
cavities. The sidecar's `shells` key restores that grouping, which in turn enables truthful
`ST_3DNumShells` and the interior-shell winding check of §8.3.

This keeps `duckdb-3d` ignorant of CityJSON files, LoD selection, and semantic surfaces —
all of which stay upstream.

An **experimental** arrow-native ingestion path (`ST_3DFromArrowNative` and siblings) reads
nested `LIST`/`STRUCT` boundary columns directly, skipping WKB serialization entirely while
producing the identical payload. It is part of a cross-repo experiment with `cityparquet-rs`
and `duckdb-cityjson` and is **not** part of the settled v1 surface.

---

## 8. Validation & measurement semantics

The definitions below are contract, not implementation notes — they are what the numbers
*mean*.

### 8.1 The three checks

- **Closed** — every undirected edge is referenced exactly twice, with opposing local
  orientation.
- **Manifold** — no undirected edge belongs to more than two incident faces, and local
  connectivity does not branch.
- **Oriented** — face winding is consistent within each shell, *and* interior shells are
  wound opposite the exterior shell.

A face is **degenerate** if fewer than 3 distinct vertices survive normalization,
triangulation fails, or its area is zero within tolerance.

Validation runs at import and its result is cached in the payload, so the predicates are
reads rather than recomputation.

`ST_3DVolume` requires all three checks plus zero degenerate faces; `ST_3DSurfaceArea`
requires only the absence of degenerate faces. Both **raise** when unmet (§2.1).

**Tolerance** is a small floating-point epsilon for repeated-point and near-zero-area tests,
defined as named constants in `src/kernel/core_types.hpp`.

The near-zero-area test is **absolute** (`kEpsAbsolute`, 1e-12), which is only defensible
because the quantity it measures is computed about a local reference point. Newell's area
vector pairs a coordinate difference with a coordinate sum, so on absolute projected
coordinates a face of exactly zero area returns not 0 but noise of order
`n·eps·|position|·|extent|` — 1e-11 to 1e-10 at RD New easting/northing, i.e. one to two
orders *above* the threshold. The verdict would then depend on where the building sits, and
because `degenerate_face_count` gates `ST_3DVolume` and `ST_3DSurfaceArea`, so would a hard
error. `NewellRingAreaVector` therefore references the ring's own first vertex, which is
exact (the area vector is translation-invariant) and returns 0 for a flat face at any
magnitude. Do not restore the absolute form, and do not "fix" the resulting sensitivity by
loosening the epsilon — that would change which real buildings are accepted.

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

Because each shell is separately closed (`ComputeVolume` refuses otherwise), giving each its
own reference point is exact and does not disturb the cavity subtraction above: shells still
accumulate **signed** into the per-solid total before `abs` is applied. `validation.cpp`'s
interior-shell winding check uses the same helper (`ShellLocalOrigin`, `geometry_math.hpp`),
so the sign it tests and the magnitude volume reports come from bit-identical arithmetic.

Do not "simplify" the reference-point subtraction away, and do not hoist it to model scope.
Both are pinned by `test/cpp/test_measurements.cpp` and `test/sql/st_3d_multisolid.test`.

### 8.3 What shell grouping actually buys

A correctly-wound cavity yields the correct volume **even without** shell metadata, because
the signed sum is driven by winding, not by grouping. What the `shells` sidecar adds is:

- truthful introspection — `ST_3DNumShells` reports 2 rather than 1; and
- the shell partition needed to *enforce* the interior-opposite-exterior invariant. A
  same-wound cavity would otherwise silently **add** its volume; with the partition available
  the check clears `is_oriented` / `is_valid`, and `ST_3DVolume` refuses.

The check is relative-only: absolute outward orientation of the exterior shell, and true
point-in-polyhedron containment, are out of scope.

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

- Performance tuning — bounding-box pre-filters for the distance family.
- Stored SRID in the payload header, enabling a one-argument `ST_3DTransform` and cross-CRS
  mismatch detection ([FUTURE_WORK.md §2](./FUTURE_WORK.md)).
- Bundling PROJ's `proj.db` into the distributable extension.
- Moving CityJSON-aware interpretation upstream ([FUTURE_WORK.md §1](./FUTURE_WORK.md)).

**Lower priority.** Remaining PostGIS-analogue accessors and serializers — `ST_HasM`, `ST_M`,
`ST_Zmflag`, `ST_3DLongestLine`, `ST_Affine`, `ST_FlipCoordinates`, `ST_SwapOrdinates`,
`ST_PointOnSurface`, `ST_Boundary`, `ST_AsX3D`, `ST_AsGML`, `ST_AsKML`.

**Deferred pending a backend decision.** Everything requiring robust exact arithmetic: 3D
booleans (union / difference / intersection), true 3D convex hulls, tessellation, straight
skeletons, medial axes, and topology-repair workflows. These are gated on whether to take on
a CGAL or SFCGAL dependency — a decision deliberately not yet made, per §2.4.

**Experimental.** Arrow-native ingestion (§7), pending the outcome of the cross-repo
experiment with `cityparquet-rs` and `duckdb-cityjson`.
