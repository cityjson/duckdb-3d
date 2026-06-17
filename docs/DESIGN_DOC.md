# DuckDB 3D Extension - Technical Design

**Document purpose**: This document specifies the technical architecture, SQL contract, binary representation, and development workflow for the `duckdb-3d` extension. It is also the single source of truth for the **post-v1 3D function roadmap** ([§16](#16-3d-function-roadmap-postgis-derived)), derived from the useful subset of PostGIS's 3D function set.

**Status**: Design baseline for v1 implementation (§1–§15); planning roadmap for subsequent functions (§16).

**Primary audience**: Engineers and coding agents implementing or reviewing the extension.

## 1. Overview

### 1.1 Purpose

`duckdb-3d` is a reusable DuckDB extension for 3D solid processing. Its job is to make 3D city-model geometries queryable in DuckDB with explicit solid-aware types and functions, rather than relying on the existing 2D/simple-features-centric geometry surface.

The extension is intentionally generic. It is not a CityJSON-only extension. CityJSON is an important upstream producer of 3D solids, but the `duckdb-3d` public API is designed so other data sources can produce compatible inputs.

### 1.2 Why This Extension Exists

DuckDB can already store binary geometry values and has a built-in `GEOMETRY` logical type, but the current geometry surface is not sufficient for solid-centric 3D workflows such as:

- calculating enclosed volume
- checking whether a solid is closed or manifold
- distinguishing solids from surface-only meshes
- preserving shell structure for city-model solids

`duckdb-3d` fills that gap by introducing a solid-specific type and a compact set of 3D validation and measurement functions.

### 1.3 Current Repo State

This repository was cloned from the standard DuckDB extension template and still contains the placeholder `quack` scaffold:

- [CMakeLists.txt](/private/tmp/duckdb-3d/CMakeLists.txt)
- [extension_config.cmake](/private/tmp/duckdb-3d/extension_config.cmake)
- [src/quack_extension.cpp](/private/tmp/duckdb-3d/src/quack_extension.cpp)

The first implementation task after this documentation phase is to rename the scaffold from `quack` to `three_d` and replace the template functions with the actual 3D extension surface described below.

## 2. Goals And Non-Goals

### 2.1 Goals For v1

- Provide a reusable DuckDB extension dedicated to 3D solids.
- Expose a public `SOLID_3D` type backed by an opaque binary payload.
- Support import from WKB polyhedral solids emitted by CityJSON and similar systems.
- Support solid validation and measurements:
  - bounds
  - face, shell, and solid counts
  - closedness
  - manifoldness
  - orientation consistency
  - surface area
  - volume
- Preserve enough topology to round-trip supported solids back to WKB.
- Keep the SQL naming style familiar to spatial users with `ST_3D*` functions.
- Standardize strict test-driven development for all future implementation work.

### 2.2 Non-Goals For v1

- 3D boolean operations such as union, difference, and intersection.
- Topology repair or silent healing of malformed solids.
- Full PostGIS feature parity.
- CGAL or SFCGAL integration in the first implementation.
- Surface-only analytics for `TIN`, `MultiSurface`, or `Polygon` data beyond explicit rejection or validation-only diagnostics.
- Deep integration inside the `cityjson` extension binary. v1 integration is SQL composition, not a hard runtime dependency.

Several of these v1 non-goals (general geometry classes, boolean operations, a CGAL/SFCGAL backend) are revisited and prioritised in the post-v1 roadmap, [§16](#16-3d-function-roadmap-postgis-derived).

## 3. Target Platform

- DuckDB runtime target: `v1.5.x`
- Extension implementation language: C++
- Extension style: out-of-tree DuckDB C++ extension using the standard `ExtensionLoader` workflow

### 3.1 Naming Note

The repository and product name remain `duckdb-3d`, but the implementation needs a legal extension symbol and load name.

Assumption for v1:

- repository name: `duckdb-3d`
- internal extension target and entrypoint name: `three_d`
- SQL function family: `ST_3D*`

Reason:

- the DuckDB C++ extension entry macro expands to a C++ symbol with the extension name token, which requires a valid identifier
- an unquoted SQL `LOAD 3d` form is also awkward to rely on as the primary workflow

If a later packaging strategy supports a clean alias to `3d`, it can be added without changing the core SQL function names.

## 4. Public Type Model

### 4.1 `SOLID_3D`

`SOLID_3D` is the primary public type in v1.

Design decision:

- SQL-visible type name: `SOLID_3D`
- Logical storage base: `BLOB`
- Registration strategy: register a named type alias over a `BLOB` logical type with the alias `SOLID_3D`
- Interpretation: only the `duckdb-3d` extension interprets the payload contents

Rationale:

- DuckDB extension type registration works well for named logical types, but v1 does not require a new storage primitive.
- A versioned opaque payload gives strong control over topology preservation and forward compatibility.
- The type remains efficient for vectorized execution and cached materialization.

### 4.2 Null Semantics

All public scalar functions follow DuckDB’s standard null propagation rules:

- if any required argument is `NULL`, the result is `NULL`
- `TRY` constructors return `NULL` instead of raising on unsupported or invalid input
- non-`TRY` constructors raise descriptive runtime errors

## 5. Public SQL API

## 5.1 Constructor And Export Functions

| Function | Signature | Behavior |
| --- | --- | --- |
| `ST_3DFromWKB` | `ST_3DFromWKB(wkb BLOB)` | Import a supported WKB solid using only WKB topology. |
| `ST_3DFromWKB` | `ST_3DFromWKB(wkb BLOB, geometry_properties VARCHAR)` | Import a supported WKB solid and use JSON text metadata when provided to recover CityJSON-specific structure. |
| `ST_3DTryFromWKB` | `ST_3DTryFromWKB(wkb BLOB)` | Same as `ST_3DFromWKB`, but returns `NULL` on failure. |
| `ST_3DTryFromWKB` | `ST_3DTryFromWKB(wkb BLOB, geometry_properties VARCHAR)` | Same as above with metadata-aware import. |
| `ST_3DAsWKB` | `ST_3DAsWKB(solid SOLID_3D)` | Export the canonicalized solid to supported WKB. |

### 5.1.1 Constructor Rules

`ST_3DFromWKB` performs these steps:

1. Parse WKB.
2. Verify the geometry class is supported in v1.
3. Build the canonical in-memory solid model.
4. Compute cached counts, bounds, and validation flags.
5. Serialize the canonical model into the `SOLID_3D` payload.

`ST_3DFromWKB` does not silently repair invalid geometry.

Allowed constructor-side normalization:

- removal of duplicate consecutive ring vertices
- removal of WKB closing vertex duplication when reconstructing ring index lists
- canonical storage ordering for offsets and header metadata

Disallowed constructor-side repair:

- closing open shells
- merging dangling edges
- flipping face winding to force a valid orientation
- patching non-manifold topology

### 5.1.2 Supported Input Classes In v1

Supported:

- `PolyhedralSurface Z`
- `GeometryCollection Z` where every child is `PolyhedralSurface Z`

Conditionally supported with metadata:

- CityJSON-derived `Solid` values represented as `PolyhedralSurface Z` plus `geometry_properties` with shell hierarchy information

Unsupported in v1:

- `Polygon Z`
- `MultiPolygon Z`
- `TIN Z`
- `Triangle Z`
- arbitrary `GeometryCollection Z`
- surface-only geometry that is not a solid candidate

### 5.1.3 Important Shell-Grouping Constraint

WKB `PolyhedralSurface Z` does not by itself preserve CityJSON shell grouping. Therefore:

- plain `ST_3DFromWKB(wkb)` imports a `PolyhedralSurface Z` as a single solid with a single shell
- plain `GeometryCollection Z` imports as a collection of single-shell solids
- richer shell grouping is only available when supplemental metadata is passed

For CityJSON integration, the preferred constructor is:

```sql
ST_3DFromWKB(geometry, geometry_properties)
```

This gives `duckdb-3d` access to CityJSON structure not recoverable from WKB alone.

### 5.1.4 Export Rules

`ST_3DAsWKB` exports:

- a single-solid value as `PolyhedralSurface Z`
- a multi-solid value as `GeometryCollection Z` of `PolyhedralSurface Z`

The exported WKB is canonicalized from the internal representation. It is not guaranteed to be byte-for-byte identical to the input WKB.

## 5.2 Introspection And Diagnostics

| Function | Signature | Behavior |
| --- | --- | --- |
| `ST_3DBounds` | `ST_3DBounds(solid SOLID_3D)` | Return the 3D bounding box. |
| `ST_3DNumSolids` | `ST_3DNumSolids(solid SOLID_3D)` | Return the number of solids. |
| `ST_3DNumShells` | `ST_3DNumShells(solid SOLID_3D)` | Return the total number of shells across all solids. |
| `ST_3DNumFaces` | `ST_3DNumFaces(solid SOLID_3D)` | Return the total number of polygon faces across all shells. |
| `ST_3DIsClosed` | `ST_3DIsClosed(solid SOLID_3D)` | Return whether every shell is closed. |
| `ST_3DIsManifold` | `ST_3DIsManifold(solid SOLID_3D)` | Return whether every shell is edge-manifold. |
| `ST_3DIsOriented` | `ST_3DIsOriented(solid SOLID_3D)` | Return whether shell and face orientation is internally consistent. |
| `ST_3DValidationReport` | `ST_3DValidationReport(solid SOLID_3D)` | Return a structured validation report. |

### 5.2.1 `ST_3DBounds` Return Type

```sql
STRUCT(
    min_x DOUBLE,
    min_y DOUBLE,
    min_z DOUBLE,
    max_x DOUBLE,
    max_y DOUBLE,
    max_z DOUBLE
)
```

### 5.2.2 `ST_3DValidationReport` Return Type

```sql
STRUCT(
    is_valid BOOLEAN,
    is_closed BOOLEAN,
    is_manifold BOOLEAN,
    is_oriented BOOLEAN,
    solid_count BIGINT,
    shell_count BIGINT,
    face_count BIGINT,
    open_edge_count BIGINT,
    non_manifold_edge_count BIGINT,
    degenerate_face_count BIGINT,
    orientation_error_count BIGINT,
    code VARCHAR,
    message VARCHAR
)
```

Return contract:

- `is_valid` is `true` only when the value is closed, manifold, oriented, and free of degenerate faces
- `code` is a stable machine-readable summary code
- `message` is a human-readable diagnostic summary

## 5.3 Measurement Functions

| Function | Signature | Behavior |
| --- | --- | --- |
| `ST_3DSurfaceArea` | `ST_3DSurfaceArea(solid SOLID_3D)` | Return total polygon surface area. |
| `ST_3DVolume` | `ST_3DVolume(solid SOLID_3D)` | Return total enclosed volume. |

### 5.3.1 Measurement Preconditions

`ST_3DSurfaceArea` requires:

- the solid payload to be structurally parseable
- no degenerate faces

`ST_3DVolume` requires:

- `is_closed = true`
- `is_manifold = true`
- `is_oriented = true`
- `degenerate_face_count = 0`

If the preconditions are not met:

- `ST_3DSurfaceArea` raises
- `ST_3DVolume` raises

Users can inspect `ST_3DValidationReport` first when they need failure diagnostics.

### 5.3.2 Volume Semantics

- Volume is reported as a non-negative `DOUBLE`.
- For multi-solid values, the result is the sum of member-solid volumes.
- Interior-shell subtraction is supported only when shell grouping is available in the canonical model.
- For plain WKB imports without shell metadata, v1 treats a single `PolyhedralSurface Z` as a one-shell solid.

## 6. Canonical Solid Model

The canonical solid model must preserve enough information for:

- validation
- measurements
- counts and bounds
- WKB export
- future backend replacement without SQL changes

### 6.1 Core Topology

The canonical model stores:

- unique vertex array in transformed 3D coordinates
- solid-to-shell offsets
- shell-to-face offsets
- face-to-ring offsets
- ring-to-vertex-index offsets
- ring vertex indices into the vertex array

This preserves original polygonal topology instead of flattening everything into triangles.

### 6.2 Derived Geometry Cache

To support validation and measurements efficiently, the payload also stores derived caches:

- per-face triangulation ranges
- triangle vertex indices
- cached 3D bounding box
- cached topology validity flags

Triangulation is a derived cache, not the primary source of truth.

### 6.3 Coordinate Semantics

All internal coordinates are stored as transformed `DOUBLE` XYZ values.

`duckdb-3d` does not store CityJSON transform metadata in the `SOLID_3D` payload. Upstream producers such as `cityjson` must already apply any source transform before or during WKB construction.

## 7. Binary Payload Format

`SOLID_3D` uses a versioned binary format.

### 7.1 Header

Header fields:

- magic bytes: `D3DS`
- format version: `u16 major`, `u16 minor`
- flags: `u32`
- vertex count: `u32`
- solid count: `u32`
- shell count: `u32`
- face count: `u32`
- ring count: `u32`
- triangle count: `u32`
- bbox min/max: `6 x f64`

### 7.2 Offset Arrays

Stored in this order:

- `solid_shell_offsets[solid_count + 1]`
- `shell_face_offsets[shell_count + 1]`
- `face_ring_offsets[face_count + 1]`
- `ring_vertex_offsets[ring_count + 1]`
- `face_triangle_offsets[face_count + 1]`

### 7.3 Data Arrays

Stored in this order:

- `vertices[vertex_count]` as `x,y,z` `f64`
- `ring_vertex_indices[*]` as `u32`
- `triangle_vertex_indices[triangle_count * 3]` as `u32`

### 7.4 Validation Cache

Stored after geometry arrays:

- `open_edge_count`
- `non_manifold_edge_count`
- `degenerate_face_count`
- `orientation_error_count`
- summary flags:
  - closed
  - manifold
  - oriented
  - valid

### 7.5 Compatibility Rules

- v1 readers must reject unknown major versions.
- v1 readers may accept newer minor versions only if all required sections are present and compatible.
- Any breaking layout change requires a major version bump.

## 8. Import Pipeline

## 8.1 WKB Parsing

The import layer parses:

- byte order
- geometry type
- polygon rings
- geometry-collection nesting when present

For v1, the parser does not attempt to ingest general simple-features geometry into solids.

## 8.2 Metadata-Aware Import

When `geometry_properties` is provided, the import layer may use it to recover structure unavailable in plain WKB.

The second argument is JSON text stored in a `VARCHAR`, matching the current `cityjson` extension contract for `geometry_properties`.

Expected CityJSON-related metadata fields include:

- `type`
- `cityjsonType`
- `lod`
- `children`
- `shellCount`
- `solidCount`
- `semantics`

If metadata conflicts with WKB:

- `ST_3DFromWKB` raises
- `ST_3DTryFromWKB` returns `NULL`

### 8.2.1 Current CityJSON Constraint

The current CityJSON extension preserves useful shell hierarchy for `Solid`, but its `geometry_properties` payload is not yet rich enough to guarantee full shell grouping for all `MultiSolid` and `CompositeSolid` cases with interior shells.

Therefore v1 `duckdb-3d` support is defined as follows:

- `Solid` with plain WKB: supported as one-shell solid
- `Solid` with CityJSON shell metadata: supported with recovered shell grouping where the metadata is sufficient
- `MultiSolid` or `CompositeSolid` with plain WKB: supported only as a collection of one-shell solids
- `MultiSolid` or `CompositeSolid` with richer future metadata: reserved for a future compatibility extension

This limitation is explicit and must not be hidden behind silent inference.

## 9. Validation Rules

Validation is part of import and report generation.

### 9.1 Closedness

A shell is closed when every undirected edge is referenced exactly twice with opposing local orientation.

### 9.2 Manifoldness

A shell is manifold when no undirected edge belongs to more than two incident faces and local connectivity does not create branching topology.

### 9.3 Orientation Consistency

Orientation checks ensure:

- each shell has consistent face winding
- interior shells, when present, are oriented opposite to the exterior shell

v1 does not silently correct orientation.

### 9.4 Degenerate Faces

Faces are degenerate if:

- fewer than 3 distinct vertices remain after normalization
- triangulation fails because the face is not a valid polygon in 3D
- computed area is zero within tolerance

### 9.5 Tolerance Policy

v1 uses a small floating-point epsilon for repeated-point and near-zero-area checks. The epsilon is an implementation constant and must be documented in code when introduced.

## 10. Measurement Rules

### 10.1 Surface Area

Surface area is computed as the sum of triangle areas over the derived face triangulation cache.

### 10.2 Volume

Volume is computed by summing signed tetrahedral contributions over oriented triangles.

Rules:

- exterior shells contribute positive signed volume
- interior shells contribute negative signed volume
- multi-solid values sum member-solid contributions

If shell roles are unavailable, v1 only computes volume for one-shell solids.

## 11. DuckDB Integration Architecture

The extension will be split into these implementation layers:

### 11.1 Registration Layer

Responsibilities:

- register `SOLID_3D`
- register constructor, export, validation, and measurement scalar functions
- register casts if added in future phases

### 11.2 SQL Function Layer

Responsibilities:

- bind arguments
- decode the `SOLID_3D` payload
- dispatch to import, validation, or measurement services
- produce vectorized results

### 11.3 Kernel Layer

Responsibilities:

- WKB parsing
- canonical model construction
- triangulation
- topology analysis
- measurements
- binary serialization and deserialization

### 11.4 Error Layer

Responsibilities:

- stable error codes
- user-facing exception text
- conversion to `TRY` null-return behavior

## 12. CityJSON Interoperability Contract

v1 integration with `duckdb-cityjson-extension` is SQL composition.

Preferred usage:

```sql
LOAD cityjson;
LOAD three_d;

WITH objects AS (
    SELECT id, geometry, geometry_properties
    FROM read_cityjson('buildings.city.json', lod => '2.2')
)
SELECT
    id,
    ST_3DVolume(ST_3DFromWKB(geometry, geometry_properties)) AS volume
FROM objects
WHERE geometry IS NOT NULL;
```

Contract assumptions:

- `cityjson` supplies WKB as a `BLOB`
- `cityjson` supplies sidecar `geometry_properties` JSON text in a `VARCHAR`
- `duckdb-3d` does not need to know about CityJSON files or tables directly

Future integration work may improve metadata richness, but v1 keeps the extensions decoupled.

## 13. Development Workflow

This repository follows strict test-driven development.

Every feature and bug fix must use the red-green-refactor cycle:

1. write a failing test
2. implement the smallest change needed to make it pass
3. refactor while keeping the tests green

### 13.1 Mandatory TDD Rules

- No new public function is considered started until a failing test exists.
- Every bug fix starts with a regression test that fails before the fix.
- Refactors must be behavior-preserving and covered by existing tests.
- Large speculative implementation batches are not allowed.

### 13.2 Test Placement

- `test/cpp/`:
  - WKB parser tests
  - canonical model tests
  - validation tests
  - measurement math tests
  - binary format round-trip tests
- `test/sql/`:
  - SQL binding behavior
  - null propagation
  - `TRY` behavior
  - function contract tests
  - `cityjson` interoperability smoke tests when the dependency is available

### 13.3 Preferred Test Order

1. unit test first
2. SQL integration test second
3. implementation third
4. refactor last

## 14. Initial Implementation Phases

### Phase 1

- rename scaffold from `quack` to `three_d`
- register `SOLID_3D`
- implement payload serializer/deserializer
- implement `ST_3DFromWKB`, `ST_3DTryFromWKB`, `ST_3DAsWKB`
- implement bounds and count functions

### Phase 2

- implement validation engine
- implement `ST_3DIsClosed`
- implement `ST_3DIsManifold`
- implement `ST_3DIsOriented`
- implement `ST_3DValidationReport`

### Phase 3

- implement triangulation cache
- implement `ST_3DSurfaceArea`
- implement `ST_3DVolume`

### Phase 4

- performance tuning
- richer metadata-aware import
- improved interoperability with CityJSON multi-shell and multi-solid cases

### Deferred Future Phase

- evaluate optional CGAL or SFCGAL backend
- boolean operations
- repair workflows
- broader geometry-class coverage

The concrete function backlog for this phase — prioritised `must`/`should`/`want` with per-function I/O specifications — is maintained in [§16](#16-3d-function-roadmap-postgis-derived).

## 15. Implementation Acceptance Criteria

The implementation phase may begin when contributors agree that this document is stable enough to act as the source of truth.

Any future implementation is complete only when:

- the SQL contract matches this design or the design is updated intentionally
- tests exist before implementation changes
- docs and tests are updated together
- no silent topology repair has been introduced
- binary format changes are versioned explicitly

## 16. 3D Function Roadmap (PostGIS-derived)

This section is a prioritised, implementable specification of the 3D functions
`duckdb-3d` will support beyond v1, derived from the useful subset of PostGIS's 3D
function set (reference:
<https://postgis.net/docs/PostGIS_Special_Functions_Index.html>). Each `must`/`should`
function carries an I/O specification detailed enough to implement one at a time under the
TDD workflow of §13.

Items here are **not yet implemented** unless marked **✅ implemented** in the tables below
or listed in [§16.9](#169-implemented-functions-postgis-analogues). Everything in §1–§15
remains the architectural source of truth; where this roadmap and those sections ever
disagree, §1–§15 win and this section is corrected.

**Implementation status (branch `develop`):** the `GEOM_3D` type and a first set of
class-generic accessors/transforms have landed. `SOLID_3D`-only functions already
working are `ST_Area`, `ST_3DPerimeter`, `ST_3DArea`; class-generic functions now
available on both `SOLID_3D` and `GEOM_3D` are `ST_NDims`, `ST_HasZ`, `ST_ZMin`,
`ST_ZMax`, `ST_Translate`, `ST_Scale`, `ST_RotateX/Y/Z`. `GEOM_3D`-only accessors
`ST_X`, `ST_Y`, `ST_Z`, `ST_CoordDim`, `ST_GeometryType`, `ST_Dimension`,
`ST_NumGeometries`, and the measurement `ST_3DLength` are also implemented. See
[§16.9](#169-implemented-functions-postgis-analogues).
The `GEOM_3D` WKB parser now covers `Polygon Z`, `MultiPoint Z`, `MultiPolygon Z`,
and `PolyhedralSurface Z` in addition to `Point Z`, `LineString Z`, and
`MultiLineString Z`. The `must` distance pair `ST_3DDistance` / `ST_3DDWithin` is
implemented on `GEOM_3D` (kernel: `src/kernel/geom_distance.cpp`).
The distance-family `should` functions `ST_3DMaxDistance`, `ST_3DDFullyWithin`, and
`ST_3DIntersects` are implemented on `GEOM_3D`, and `ST_3DExtrude` (footprint → LoD1
solid) is implemented (kernel: `src/kernel/geom_construct.cpp`).
`ST_IsPlanar` and `ST_3DCentroid` are implemented on `GEOM_3D`
(kernel: `src/kernel/geom_analysis.cpp`).
Remaining work: add the remaining `should` transforms and
construction functions (`ST_Force3D`, `ST_ConvexHull`), the distance witness
functions (`ST_3DClosestPoint`, `ST_3DShortestLine`), and serialization
functions (`ST_AsText`, `ST_AsGeoJSON`, `ST_AsBinary`).

On naming: we keep `ST_*` / `ST_3D*` names for familiarity, but per §2.2 **full PostGIS
parity is a non-goal** — this is a curated subset chosen for 3D city-model workflows.

### 16.1 Scope And Priorities

#### 16.1.1 Priority Anchor

Priorities are anchored on **3D city-model / building workflows** (CityJSON →
CityParquet / CityLake): enclosed volume, building height, footprint area, solid
validity, and building-to-building proximity. General-purpose PostGIS coverage and
implementation ease are secondary tie-breakers.

#### 16.1.2 Priority Legend

| Priority | Meaning |
| --- | --- |
| **must** | Core building-model metric or query. Implementable on the self-contained kernel (no external geometry backend). Highest value-to-cost. |
| **should** | Broadens coverage meaningfully. Still no external backend. |
| **want** | Lower demand, **or** requires a CGAL/SFCGAL backend (flagged). Deferred until the backend question is settled or demand appears. |

#### 16.1.3 Backend Rule

Nothing marked `must` or `should` may require CGAL/SFCGAL. Every robust 3D boolean,
hull, general extrusion, skeleton, or medial-axis operation is `want` and explicitly
flagged, consistent with §2.2 and the §14 deferred phase.

### 16.2 Prerequisite: A General 3D Geometry Type (`GEOM_3D`)

The v1 public type, `SOLID_3D`, models **closed polyhedral solids** only (§4.1). Most
PostGIS accessors, transforms, and distance functions operate on *arbitrary* geometries
(points, lines, polygons, surfaces). Supporting them requires a second public type.

**Status**: implemented.

- A named type **`GEOM_3D`** is registered as a BLOB-backed alias (same registration strategy
  as `SOLID_3D`, §4.1). It carries `Point Z`, `LineString Z`, `MultiLineString Z`,
  `Polygon Z`, `MultiPoint Z`, `MultiPolygon Z`, and `PolyhedralSurface Z`. In the
  `GeomModel`, `ring_offsets` partitions vertices into rings (Polygon / MultiPolygon /
  PolyhedralSurface) and `part_offsets` partitions members — into `ring_offsets` for
  polygonal multis/surfaces, or directly into vertices for `MultiPoint`/`MultiLineString`.
- `SOLID_3D` stays the dedicated solid type. A solid is convertible to `GEOM_3D` (as its
  boundary surface); a closed/oriented/manifold `GEOM_3D` surface is convertible to
  `SOLID_3D` via `ST_MakeSolid` (not yet implemented).
- The v1 `D3DS` payload (§7) remains solid-specific. A sibling **`D3DG` payload**
  (`src/kernel/geom_payload.cpp`) carries lower-dimensional geometries — a versioned
  change under the §7.5 compatibility rules. This milestone unblocks the class-generic
  accessor and transform work below.

Functions in this section declare their accepted type(s): `GEOM_3D` for class-generic
operations, `SOLID_3D` for solid-only operations, or both. Null / error semantics are
inherited from §4.2 (any `NULL` argument → `NULL`; non-`TRY` raise; `TRY` returns `NULL`).

### 16.3 Accessors And Properties

Class-generic; operate on `GEOM_3D` (and `SOLID_3D` where a bounding box suffices).

| Function | Signature (input → output) | Priority | Backend | Notes / preconditions |
| --- | --- | --- | --- | --- |
| `ST_NDims` | `(geom GEOM_3D) → INTEGER` | must | kernel | ✅ implemented on `SOLID_3D` and `GEOM_3D`. Coordinate dimension (3 for XYZ). |
| `ST_HasZ` | `(geom GEOM_3D) → BOOLEAN` | must | kernel | ✅ implemented on `SOLID_3D` and `GEOM_3D`. True if geometry carries Z (always true in v1; future-proofs XY inputs). |
| `ST_Z` | `(point GEOM_3D) → DOUBLE` | must | kernel | ✅ implemented on `GEOM_3D`. Z of a Point; `NULL` if empty. Raises if not a Point. |
| `ST_ZMax` | `(geom GEOM_3D) → DOUBLE` | must | kernel | ✅ implemented on `SOLID_3D` and `GEOM_3D`. Max Z of bbox. Building roof elevation. |
| `ST_ZMin` | `(geom GEOM_3D) → DOUBLE` | must | kernel | ✅ implemented on `SOLID_3D` and `GEOM_3D`. Min Z of bbox. `ZMax − ZMin` = building height. |
| `ST_X` | `(point GEOM_3D) → DOUBLE` | should | kernel | ✅ implemented on `GEOM_3D`. X of a Point. Raises if not a Point. |
| `ST_Y` | `(point GEOM_3D) → DOUBLE` | should | kernel | ✅ implemented on `GEOM_3D`. Y of a Point. Raises if not a Point. |
| `ST_CoordDim` | `(geom GEOM_3D) → INTEGER` | should | kernel | ✅ implemented on `GEOM_3D`. Coordinate dimension (alias-like to `ST_NDims` in v1). |
| `ST_GeometryType` | `(geom GEOM_3D) → VARCHAR` | should | kernel | ✅ implemented on `GEOM_3D`. e.g. `ST_PolyhedralSurface`, `ST_Polygon`. |
| `ST_Dimension` | `(geom GEOM_3D) → INTEGER` | should | kernel | ✅ implemented on `GEOM_3D`. Topological dim: 0 point, 1 line, 2 surface, 3 solid. |
| `ST_NumGeometries` | `(geom GEOM_3D) → INTEGER` | should | kernel | ✅ implemented on `GEOM_3D`. Member count of a collection/multi geometry. |
| `ST_HasM` | `(geom GEOM_3D) → BOOLEAN` | want | kernel | M dimension not used by city models. |
| `ST_M` | `(point GEOM_3D) → DOUBLE` | want | kernel | M of a Point. |
| `ST_Zmflag` | `(geom GEOM_3D) → SMALLINT` | want | kernel | ZM dimensionality code. |

### 16.4 Measurement

| Function | Signature (input → output) | Priority | Backend | Notes / preconditions |
| --- | --- | --- | --- | --- |
| `ST_Area` | `(geom GEOM_3D) → DOUBLE` | must | kernel | ✅ implemented on `SOLID_3D`. **2D footprint area** = area of XY projection. Key building metric. |
| `ST_3DLength` | `(geom GEOM_3D) → DOUBLE` | should | kernel | ✅ implemented on `GEOM_3D`. 3D length of (multi)linestrings; 0 for areal/point. |
| `ST_3DPerimeter` | `(geom GEOM_3D) → DOUBLE` | should | kernel | ✅ implemented on `SOLID_3D`. Total length of boundary edges (used by exactly one face); 0 for closed solids. |
| `ST_3DArea` | `(geom GEOM_3D) → DOUBLE` | should | kernel | ✅ implemented on `SOLID_3D`. 3D surface area for surfaces; alias-aligned with existing `ST_3DSurfaceArea` (§5.3). |
| `ST_3DVolume` | `(solid SOLID_3D) → DOUBLE` | — | kernel | **Implemented** (§5.3, §16.9). |
| `ST_3DSurfaceArea` | `(solid SOLID_3D) → DOUBLE` | — | kernel | **Implemented** (§5.3, §16.9). |

### 16.5 Distance And Spatial Relationships

The proximity primitives for building-to-building queries.

| Function | Signature (input → output) | Priority | Backend | Notes / preconditions |
| --- | --- | --- | --- | --- |
| `ST_3DDistance` | `(g1 GEOM_3D, g2 GEOM_3D) → DOUBLE` | must | kernel | ✅ implemented. Minimum 3D cartesian distance. 0 if they intersect. |
| `ST_3DDWithin` | `(g1 GEOM_3D, g2 GEOM_3D, dist DOUBLE) → BOOLEAN` | must | kernel | ✅ implemented. True if `ST_3DDistance ≤ dist`; negative `dist` → false. |
| `ST_3DMaxDistance` | `(g1 GEOM_3D, g2 GEOM_3D) → DOUBLE` | should | kernel | ✅ implemented. Maximum 3D distance between geometries (vertex/vertex sweep). |
| `ST_3DDFullyWithin` | `(g1 GEOM_3D, g2 GEOM_3D, dist DOUBLE) → BOOLEAN` | should | kernel | ✅ implemented. True if `ST_3DMaxDistance ≤ dist`; negative `dist` → false. |
| `ST_3DIntersects` | `(g1 GEOM_3D, g2 GEOM_3D) → BOOLEAN` | should | kernel | ✅ implemented. True when minimum 3D distance is 0 within tolerance (touching counts). |
| `ST_3DClosestPoint` | `(g1 GEOM_3D, g2 GEOM_3D) → GEOM_3D` | should | kernel | 3D point on `g1` closest to `g2`. |
| `ST_3DShortestLine` | `(g1 GEOM_3D, g2 GEOM_3D) → GEOM_3D` | should | kernel | 3D shortest line between geometries. |
| `ST_3DLongestLine` | `(g1 GEOM_3D, g2 GEOM_3D) → GEOM_3D` | want | kernel | 3D longest line. |

### 16.6 Transformations And Construction

| Function | Signature (input → output) | Priority | Backend | Notes / preconditions |
| --- | --- | --- | --- | --- |
| `ST_Translate` | `(geom GEOM_3D, dx DOUBLE, dy DOUBLE, dz DOUBLE) → GEOM_3D` | must | kernel | ✅ implemented on `SOLID_3D` and `GEOM_3D`. Placement / georeferencing. Topology preserved. |
| `ST_Scale` | `(geom GEOM_3D, sx DOUBLE, sy DOUBLE, sz DOUBLE) → GEOM_3D` | should | kernel | ✅ implemented on `SOLID_3D` and `GEOM_3D`. Scale about origin. |
| `ST_RotateX` | `(geom GEOM_3D, radians DOUBLE) → GEOM_3D` | should | kernel | ✅ implemented on `SOLID_3D` and `GEOM_3D`. Rotate about X axis. |
| `ST_RotateY` | `(geom GEOM_3D, radians DOUBLE) → GEOM_3D` | should | kernel | ✅ implemented on `SOLID_3D` and `GEOM_3D`. Rotate about Y axis. |
| `ST_RotateZ` | `(geom GEOM_3D, radians DOUBLE) → GEOM_3D` | should | kernel | ✅ implemented on `SOLID_3D` and `GEOM_3D`. Rotate about Z axis. |
| `ST_Force3D` / `ST_Force3DZ` | `(geom GEOM_3D) → GEOM_3D` | should | kernel | Coerce 2D input to XYZ (Z=0 default). |
| `ST_3DExtrude` | `(polygon GEOM_3D, height DOUBLE) → SOLID_3D` | should | kernel | ✅ implemented. **Vertical** prism extrusion (footprint → LoD1 box); footprint normalised to CCW, result validated closed+oriented. Returns plain BLOB. |
| `ST_MakeSolid` | `(geom GEOM_3D) → SOLID_3D` | should | kernel | ✅ implemented. Cast a closed/oriented/manifold `PolyhedralSurface` to a solid; raises if not solid-eligible (no repair). Returns plain BLOB. |
| `ST_3DCentroid` | `(geom GEOM_3D) → GEOM_3D` | should | kernel | ✅ implemented. Dimension-aware centroid: vertex average (points), length-weighted (lines), area-weighted (surfaces). |
| `ST_ConvexHull` | `(geom GEOM_3D) → GEOM_3D` | should | kernel | **2D** convex hull (XY). 3D hull is `want`/CGAL (§16.8). |
| `ST_IsPlanar` | `(geom GEOM_3D) → BOOLEAN` | should | kernel | ✅ implemented. Whether all faces/rings are planar within tolerance (§9.5). City-model QA. |
| `ST_Affine` | `(geom GEOM_3D, a..l DOUBLE ×12) → GEOM_3D` | want | kernel | General 3D affine; rarely called directly. |
| `ST_FlipCoordinates` | `(geom GEOM_3D) → GEOM_3D` | want | kernel | Swap X/Y. |
| `ST_SwapOrdinates` | `(geom GEOM_3D, spec VARCHAR) → GEOM_3D` | want | kernel | Swap named ordinates. |
| `ST_PointOnSurface` | `(geom GEOM_3D) → GEOM_3D` | want | kernel | Guaranteed on-surface point. |
| `ST_Boundary` | `(geom GEOM_3D) → GEOM_3D` | want | kernel | Topological boundary. |

### 16.7 Serialization / Output

| Function | Signature (input → output) | Priority | Backend | Notes / preconditions |
| --- | --- | --- | --- | --- |
| `ST_AsText` | `(geom GEOM_3D) → VARCHAR` | should | kernel | ISO WKT (with Z). Inspection / debugging. |
| `ST_AsGeoJSON` | `(geom GEOM_3D) → VARCHAR` | should | kernel | GeoJSON; note GeoJSON has limited solid support. |
| `ST_AsBinary` | `(geom GEOM_3D) → BLOB` | should | kernel | OGC/ISO WKB (complements solid-only `ST_3DAsWKB`, §5.1). |
| `ST_AsX3D` | `(geom GEOM_3D) → VARCHAR` | want | kernel | X3D XML for 3D viewers. |
| `ST_AsGML` | `(geom GEOM_3D) → VARCHAR` | want | kernel | GML output. |
| `ST_AsKML` | `(geom GEOM_3D) → VARCHAR` | want | kernel | KML output. |

### 16.8 CGAL / SFCGAL Backend Cluster (all `want`, flagged)

All functions in this subsection require a robust exact-arithmetic 3D geometry backend
(CGAL/SFCGAL). Per §2.2 and the §14 deferred phase, this backend is **deferred**; these
are documented as one cluster, gated on a future backend decision. PostGIS exposes most of
these under both `ST_3D*` and `CG_*` names.

| Function (PostGIS `CG_*` alias) | Signature (input → output) | Notes |
| --- | --- | --- |
| `ST_3DUnion` (`CG_3DUnion`) | `(a SOLID_3D, b SOLID_3D) → SOLID_3D` | Boolean union. |
| `ST_3DDifference` (`CG_3DDifference`) | `(a SOLID_3D, b SOLID_3D) → SOLID_3D` | Boolean difference. |
| `ST_3DIntersection` (`CG_3DIntersection`) | `(a SOLID_3D, b SOLID_3D) → SOLID_3D` | Boolean intersection. |
| `ST_3DConvexHull` (`CG_3DConvexHull`) | `(geom GEOM_3D) → SOLID_3D` | True 3D convex hull. |
| `ST_Extrude` (`CG_Extrude`) | `(geom GEOM_3D, vx, vy, vz DOUBLE) → SOLID_3D` | General (non-vertical) extrusion. |
| `ST_3DAlphaWrapping` (`CG_3DAlphaWrapping`) | `(geom GEOM_3D, alpha, offset DOUBLE) → SOLID_3D` | Watertight wrap; model repair. |
| `ST_StraightSkeleton` (`CG_StraightSkeleton`) | `(geom GEOM_3D) → GEOM_3D` | Roof-skeleton style operations. |
| `ST_ApproximateMedialAxis` (`CG_ApproximateMedialAxis`) | `(geom GEOM_3D) → GEOM_3D` | Medial axis. |
| `ST_Tesselate` (`CG_Tesselate`) | `(geom GEOM_3D) → GEOM_3D` | Surface tessellation. |

### 16.9 Implemented Functions (PostGIS Analogues)

#### 16.9.1 v1 baseline

The 14 functions specified in §5 exist today (`src/three_d_extension.cpp`) and define the
baseline this roadmap extends. Listed here with their nearest PostGIS analogue.

| `duckdb-3d` function | Signature | PostGIS analogue |
| --- | --- | --- |
| `ST_3DFromWKB` | `(wkb BLOB[, props VARCHAR]) → SOLID_3D` | `ST_GeomFromWKB` (solid-specialised) |
| `ST_3DTryFromWKB` | `(wkb BLOB[, props VARCHAR]) → SOLID_3D` (NULL on failure) | `ST_GeomFromWKB` + TRY |
| `ST_3DAsWKB` | `(solid SOLID_3D) → BLOB` | `ST_AsBinary` |
| `ST_3DBounds` | `(solid SOLID_3D) → STRUCT(min/max x,y,z DOUBLE)` | `Box3D` / `ST_3DExtent` |
| `ST_3DNumSolids` | `(solid SOLID_3D) → BIGINT` | `ST_NumGeometries` (solid-scoped) |
| `ST_3DNumShells` | `(solid SOLID_3D) → BIGINT` | `ST_NumInteriorRings` (3D analogue) |
| `ST_3DNumFaces` | `(solid SOLID_3D) → BIGINT` | `ST_NPatches` |
| `ST_3DIsClosed` | `(solid SOLID_3D) → BOOLEAN` | `ST_IsClosed` |
| `ST_3DIsManifold` | `(solid SOLID_3D) → BOOLEAN` | (SFCGAL validity) |
| `ST_3DIsOriented` | `(solid SOLID_3D) → BOOLEAN` | (SFCGAL orientation) |
| `ST_3DValidationReport` | `(solid SOLID_3D) → STRUCT(...)` | `ST_IsValidDetail` (3D analogue) |
| `ST_3DSurfaceArea` | `(solid SOLID_3D) → DOUBLE` | `ST_3DArea` / `CG_3DArea` |
| `ST_3DVolume` | `(solid SOLID_3D) → DOUBLE` | `ST_Volume` / `CG_Volume` |

#### 16.9.2 Roadmap functions delivered (branch `develop`)

Implemented on `SOLID_3D` and, where class-generic, on `GEOM_3D`. New kernel helpers:
`ComputeFootprintArea`, `ComputePerimeter` (`src/kernel/measurements.cpp`); the
primitive-distance kernel `Geom3DDistance` and its point/segment/triangle primitives
(`src/kernel/geom_distance.cpp`); the analysis helpers `Geom3DIsPlanar` and
`Geom3DCentroid` (`src/kernel/geom_analysis.cpp`).

| `duckdb-3d` function | Signature | Priority | PostGIS analogue |
| --- | --- | --- | --- |
| `ST_NDims` | `(solid SOLID_3D \| geom GEOM_3D) → INTEGER` | must | `ST_NDims` |
| `ST_HasZ` | `(solid SOLID_3D \| geom GEOM_3D) → BOOLEAN` | must | `ST_HasZ` |
| `ST_ZMin` | `(solid SOLID_3D \| geom GEOM_3D) → DOUBLE` | must | `ST_ZMin` |
| `ST_ZMax` | `(solid SOLID_3D \| geom GEOM_3D) → DOUBLE` | must | `ST_ZMax` |
| `ST_Area` | `(solid SOLID_3D) → DOUBLE` | must | `ST_Area` (footprint) |
| `ST_3DPerimeter` | `(solid SOLID_3D) → DOUBLE` | should | `ST_3DPerimeter` |
| `ST_3DArea` | `(solid SOLID_3D) → DOUBLE` | should | `ST_3DArea` / `CG_3DArea` |
| `ST_Translate` | `(solid SOLID_3D \| geom GEOM_3D, dx, dy, dz DOUBLE) → same type` | must | `ST_Translate` |
| `ST_Scale` | `(solid SOLID_3D \| geom GEOM_3D, sx, sy, sz DOUBLE) → same type` | should | `ST_Scale` |
| `ST_RotateX` | `(solid SOLID_3D \| geom GEOM_3D, radians DOUBLE) → same type` | should | `ST_RotateX` |
| `ST_RotateY` | `(solid SOLID_3D \| geom GEOM_3D, radians DOUBLE) → same type` | should | `ST_RotateY` |
| `ST_RotateZ` | `(solid SOLID_3D \| geom GEOM_3D, radians DOUBLE) → same type` | should | `ST_RotateZ` |
| `ST_Z` | `(point GEOM_3D) → DOUBLE` | must | `ST_Z` |
| `ST_X` | `(point GEOM_3D) → DOUBLE` | should | `ST_X` |
| `ST_Y` | `(point GEOM_3D) → DOUBLE` | should | `ST_Y` |
| `ST_CoordDim` | `(geom GEOM_3D) → INTEGER` | should | `ST_CoordDim` |
| `ST_GeometryType` | `(geom GEOM_3D) → VARCHAR` | should | `ST_GeometryType` |
| `ST_Dimension` | `(geom GEOM_3D) → INTEGER` | should | `ST_Dimension` |
| `ST_NumGeometries` | `(geom GEOM_3D) → INTEGER` | should | `ST_NumGeometries` |
| `ST_3DLength` | `(geom GEOM_3D) → DOUBLE` | should | `ST_3DLength` |
| `ST_3DDistance` | `(g1 GEOM_3D, g2 GEOM_3D) → DOUBLE` | must | `ST_3DDistance` |
| `ST_3DDWithin` | `(g1 GEOM_3D, g2 GEOM_3D, dist DOUBLE) → BOOLEAN` | must | `ST_3DDWithin` |
| `ST_3DMaxDistance` | `(g1 GEOM_3D, g2 GEOM_3D) → DOUBLE` | should | `ST_3DMaxDistance` |
| `ST_3DDFullyWithin` | `(g1 GEOM_3D, g2 GEOM_3D, dist DOUBLE) → BOOLEAN` | should | `ST_3DDFullyWithin` |
| `ST_3DIntersects` | `(g1 GEOM_3D, g2 GEOM_3D) → BOOLEAN` | should | `ST_3DIntersects` |
| `ST_3DExtrude` | `(polygon GEOM_3D, height DOUBLE) → SOLID_3D (BLOB)` | should | `ST_3DExtrude` |
| `ST_MakeSolid` | `(surface GEOM_3D) → SOLID_3D (BLOB)` | should | `ST_MakeSolid` |
| `ST_IsPlanar` | `(geom GEOM_3D) → BOOLEAN` | should | `ST_IsPlanar` |
| `ST_3DCentroid` | `(geom GEOM_3D) → GEOM_3D` | should | `ST_3DCentroid` |

> Note: `GEOM_3D` is implemented as a BLOB-backed alias with a sibling `D3DG` payload
> (`src/kernel/geom_payload.cpp`). Class-generic accessors and transforms are registered
> with `BLOB`, `SOLID_3D`, and `GEOM_3D` overloads because DuckDB treats named BLOB
> aliases as distinct for function resolution.

### 16.10 Detailed I/O Specifications (`must` And `should`)

Each entry is written to be directly actionable under the TDD workflow of §13: write the
failing unit test (`test/cpp/`) and SQL contract test (`test/sql/`) first, then implement
and register in `src/three_d_extension.cpp`.

Conventions for every function below: NULL input → NULL output; type mismatches and
unsupported geometry classes raise descriptive errors unless a `TRY` variant is noted; all
coordinates are transformed `DOUBLE` XYZ (§6.3).

#### 16.10.1 Accessors — `must`

**`ST_NDims(geom GEOM_3D) → INTEGER`**
- Returns the coordinate dimensionality of the stored geometry (3 in v1).
- Errors: none beyond NULL propagation.
- Tests: unit — payload with XYZ → 3; SQL — `SELECT ST_NDims(g)` returns 3.

**`ST_HasZ(geom GEOM_3D) → BOOLEAN`**
- True when the geometry carries a Z ordinate.
- Tests: SQL — XYZ geometry → true.

**`ST_Z(point GEOM_3D) → DOUBLE`**
- Z ordinate of a single Point. `NULL` for empty point. Raises if the geometry is not a
  Point.
- Tests: unit — point (1,2,3) → 3.0; SQL — non-point input raises.

**`ST_ZMax(geom GEOM_3D) → DOUBLE`** / **`ST_ZMin(geom GEOM_3D) → DOUBLE`**
- Max/min Z from the cached bounding box (reuse the bbox computed at import, §6.2). For
  `SOLID_3D`, equals `ST_3DBounds(...).max_z` / `.min_z`.
- `ZMax − ZMin` is the canonical building-height expression; document this in the example.
- Tests: unit — known bbox; SQL — equality with `ST_3DBounds` fields.

#### 16.10.2 Measurement — `must`

**`ST_Area(geom GEOM_3D) → DOUBLE`**
- Area of the **XY projection** (footprint).
- **Chosen definition (implemented):** half the total absolute XY-projected face area —
  i.e. `0.5 · Σ_faces |projected signed area of face|`. Each up-facing and each down-facing
  face projects onto the footprint exactly once, so summing absolute projected areas counts
  the footprint twice; halving recovers it. This is exact for **vertically-simple** solids
  (every vertical line meets the boundary once above and once below), independent of the
  global orientation sign, and requires no triangulation or validity preconditions. It is
  approximate for overhangs/re-entrant profiles (documented limitation; no planar union in
  v1). Implemented in `ComputeFootprintArea` (`src/kernel/measurements.cpp`).
- Precondition: none beyond a parseable model.

#### 16.10.3 Distance — `must`

**`ST_3DDistance(g1 GEOM_3D, g2 GEOM_3D) → DOUBLE`**
- Minimum 3D cartesian distance between the two geometries; 0 if they touch/intersect.
- Implementation (delivered): each geometry is decomposed into primitive elements by
  topological dimension — points (0D), segments (1D), or triangles (2D) — and the result is
  the minimum over all element pairs. Surfaces are fan-triangulated over each face's
  **exterior** ring; interior rings (holes) are ignored for distance, a documented v1
  simplification. Segment/triangle and triangle/triangle pairs return 0 on intersection via
  a Möller–Trumbore segment/triangle test. No bbox pre-filter yet (correctness first;
  performance tuning deferred).
- Tests: unit — two points; point-to-triangle; two disjoint boxes (gap distance);
  overlapping boxes → 0.

**`ST_3DDWithin(g1 GEOM_3D, g2 GEOM_3D, dist DOUBLE) → BOOLEAN`**
- `ST_3DDistance(g1,g2) ≤ dist`. Short-circuit with a bbox-expanded-by-`dist` pre-filter
  before exact computation.
- Tests: SQL — boundary case at exactly `dist`; negative `dist` → false (or raise — pin in
  test).

#### 16.10.4 Transform — `must`

**`ST_Translate(geom GEOM_3D, dx DOUBLE, dy DOUBLE, dz DOUBLE) → GEOM_3D`**
- Adds `(dx,dy,dz)` to every vertex; topology, shell/face structure, and validation flags
  are preserved (translation cannot change closedness/manifoldness/orientation); bbox is
  shifted. Re-emit payload without recomputing validation.
- Tests: unit — translate then inverse-translate equals original within epsilon; bbox
  shifted correctly; validation flags unchanged.

#### 16.10.5 `should` Functions

For `should` functions the I/O contract is the table row in §16.3–§16.7 plus these notes:

- **`ST_X` / `ST_Y`** — mirror `ST_Z`; Point-only, raise otherwise.
- **`ST_CoordDim`, `ST_GeometryType`, `ST_Dimension`, `ST_NumGeometries`** — pure header
  reads off the payload geometry tag; no math.
- **`ST_3DLength`** — sum of segment lengths over (multi)linestrings; 0 for areal/point.
- **`ST_3DPerimeter`** — sum of ring-edge lengths over polygonal/surface boundaries.
- **`ST_3DArea`** — sum of triangle areas over the triangulation cache; equals existing
  `ST_3DSurfaceArea` for solids (register as an alias or thin wrapper).
- **`ST_3DMaxDistance` / `ST_3DDFullyWithin`** — vertex/face-pair maxima; symmetric to the
  `must` distance pair.
- **`ST_3DIntersects`** — triangle/segment/point intersection tests with bbox reject; pin
  touching-vs-overlapping semantics in tests.
- **`ST_3DClosestPoint` / `ST_3DShortestLine`** — return the witness point/segment from the
  same primitive-pair search that backs `ST_3DDistance`.
- **`ST_Scale` / `ST_RotateX/Y/Z` / `ST_Force3D`** — per-vertex affine maps; recompute
  bbox; rotation/scale may invalidate cached orientation only if scale is negative —
  recompute validation when any scale factor < 0, otherwise preserve flags.
- **`ST_3DExtrude(polygon, height)`** — build a prism: bottom ring = input polygon, top
  ring = polygon translated by `(0,0,height)`, side faces connect corresponding edges; emit
  a closed, oriented one-shell `SOLID_3D`. Validate the result (must be closed + manifold +
  oriented). The canonical LoD1-from-footprint operation. Tests: extrude unit square by
  height 2 → volume 2, closed=true.
- **`ST_MakeSolid(geom)`** — accept a `PolyhedralSurface Z` that is closed/manifold/
  oriented; produce `SOLID_3D`. Raise (or `TRY`-NULL) when validation fails — no silent
  repair (§5.1.1).
- **`ST_3DCentroid`** — volume-weighted centroid for solids, area-weighted for surfaces,
  vertex average for points/lines.
- **`ST_ConvexHull`** — 2D monotone-chain hull over XY-projected vertices; returns a
  `Polygon Z` at the min Z (document the Z convention).
- **`ST_IsPlanar`** — per-face: max point-to-best-fit-plane distance ≤ tolerance (§9.5).
- **`ST_AsText` / `ST_AsGeoJSON` / `ST_AsBinary`** — serialise from the canonical model;
  `ST_AsBinary` reuses the existing WKB export path (`src/kernel/wkb_export.cpp`).

### 16.11 Roadmap Sequencing (suggested)

1. **`GEOM_3D` type + payload** (§16.2) — gating milestone; unblocks everything
   class-generic.
2. **`must` accessors** (§16.3) and **`must` measurement/distance/transform**
   (§16.4–§16.6) — the building-model core.
3. **`should` accessors, measurement, distance** — broaden query coverage.
4. **`should` transforms + construction** (`ST_3DExtrude`, `ST_MakeSolid`, `ST_3DCentroid`).
5. **`should` serialization**.
6. **`want`** items as demand appears; the **CGAL/SFCGAL cluster** (§16.8) only after the
   backend decision in §14 is made.

Every step follows red-green-refactor (§13): failing `test/cpp/` math test and `test/sql/`
contract test first, then implementation and registration, then update of §5 and this
section together.
