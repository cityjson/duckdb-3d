# DuckDB 3D Extension - Technical Design

**Document purpose**: This document specifies the technical architecture, SQL contract, binary representation, and development workflow for the `duckdb-3d` extension.

**Status**: Design baseline for v1 implementation.

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

## 15. Implementation Acceptance Criteria

The implementation phase may begin when contributors agree that this document is stable enough to act as the source of truth.

Any future implementation is complete only when:

- the SQL contract matches this design or the design is updated intentionally
- tests exist before implementation changes
- docs and tests are updated together
- no silent topology repair has been introduced
- binary format changes are versioned explicitly
