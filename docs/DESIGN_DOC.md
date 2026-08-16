# DuckDB 3D Extension - Technical Design

**Document purpose**: This document is the reference specification for the `duckdb-3d`
extension. It describes the technical architecture, SQL contract, binary representation, and
validation/measurement semantics **as implemented today** (§1–§15), and the single source of
truth for the **forward function roadmap** ([§16](#16-3d-function-roadmap-postgis-derived)),
derived from the useful subset of PostGIS's 3D function set.

Read it as reference documentation, not as a plan: §1–§15 describe the shipped design, and
§16 separates what is already implemented from what remains on the roadmap. For a hands-on
walkthrough against real data, see [EXAMPLE.md](./EXAMPLE.md).

**Primary audience**: Engineers and coding agents implementing, extending, or reviewing the
extension.

**At a glance**: the extension loads as `three_d`, exposes two BLOB-backed logical types
(`SOLID_3D` for closed polyhedral solids, `GEOM_3D` for general 3D geometry), and a family of
`ST_*` / `ST_3D*` scalar functions for import/export, introspection, validation, measurement,
distance, transformation, and serialization.

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

The repository and product name is `duckdb-3d`, but the implementation needs a legal
extension symbol and load name, so the names in use are:

- repository name: `duckdb-3d`
- internal extension target and entrypoint name: `three_d` (`LOAD three_d;`)
- SQL function family: `ST_3D*`

Reason:

- the DuckDB C++ extension entry macro expands to a C++ symbol with the extension name token, which requires a valid identifier
- an unquoted SQL `LOAD 3d` form is also awkward to rely on as the primary workflow

#### Coexistence with `spatial` (`ST_3D*` namespace)

Every 3D operation that would otherwise share a name with DuckDB's `spatial`
extension is exposed under an `ST_3D*` name (PostGIS's convention for 3D variants)
instead of that bare generic name. This is deliberate: `spatial` registers `ST_Scale`, `ST_Area`, `ST_X`,
`ST_ZMax`, `ST_AsText`, … (as scalar functions *and* macros) on its `GEOMETRY`
type, and it registers them `ERROR_ON_CONFLICT`. If `three_d` also claimed those
names, loading both extensions in one session would abort — in *either* order,
since a name registered first makes the second registration throw. By exposing
every such name under `ST_3D*` instead, `three_d` never collides: `spatial` keeps
the generic 2D
simple-features vocabulary, `three_d` provides the 3D-solid vocabulary, and the
two load together in any order (`SOLID_3D`/`GEOM_3D` carry a distinct type alias,
so overloads never cross-bind). A handful of names that `spatial` does **not**
define (`ST_Force3D`, `ST_MakeSolid`, `ST_NDims`, `ST_CoordDim`, `ST_IsPlanar`,
`ST_Geom3DFromWKB`) are kept un-prefixed. The one non-obvious rename is the solid
footprint: `ST_3DArea` is the 3D surface area, so the 2D footprint area is
`ST_3DFootprintArea`.

If a later packaging strategy supports a clean alias to `3d`, it can be added without changing the core SQL function names.

## 4. Public Type Model

### 4.1 `SOLID_3D`

`SOLID_3D` is the dedicated type for **closed polyhedral solids**. It is complemented by
`GEOM_3D`, the general 3D geometry type ([§16.2](#162-prerequisite-a-general-3d-geometry-type-geom_3d))
that carries points, lines, polygons, multis, and polyhedral surfaces for the class-generic
accessor/transform/distance/serialization functions.

Design decision:

- SQL-visible type name: `SOLID_3D`
- Logical storage base: `BLOB`
- Registration strategy: register a named type alias over a `BLOB` logical type with the alias `SOLID_3D`
- Interpretation: only the `duckdb-3d` extension interprets the payload contents

Rationale:

- DuckDB extension type registration works well for named logical types, but v1 does not require a new storage primitive.
- A versioned opaque payload gives strong control over topology preservation and forward compatibility.
- The type remains efficient for vectorized execution and cached materialization.

**Consequence of the typed constructor return.** `SOLID_3D` and `GEOM_3D` are *alias* types
over `BLOB`, and the constructors return the alias rather than plain `BLOB`
([§5.1](#51-constructor-and-export-functions)), so generic `BLOB` consumers
(`octet_length`, `length`, …) no longer bind against constructor output, and no
`SOLID_3D`/`GEOM_3D` → `BLOB` cast is registered — `… ::BLOB` raises a conversion error
rather than reinterpreting the payload; use `ST_3DAsWKB` when a `BLOB` is genuinely wanted.
For the same reason a bare `NULL` is ambiguous for every function carrying both a typed and
a `BLOB` overload and must be written `NULL::SOLID_3D`.

**Experimental (`arrow-native-type` branch):** `ST_3DFromArrowNative`/`ST_3DTryFromArrowNative`
([§8.3](#83-arrow-native-import)) ingest nested `LIST`/`STRUCT` columns directly, bypassing WKB
entirely — but they still produce exactly this same `SOLID_3D` binary payload. The payload format
itself, `PayloadHeader`, and the version are unchanged; only a second *import* path exists
alongside WKB, not a second storage representation.

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
| `ST_3DFromWKB` | `ST_3DFromWKB(wkb BLOB, geometry_properties STRUCT)` | As above, but accepts a CityParquet `geometry_properties_lod*` STRUCT (spec §8) directly — no `to_json(...)` round-trip. Registered as `(BLOB, ANY)`: the bind routes a STRUCT to the struct executor and VARCHAR / JSON / `NULL` back to the JSON executor, so it is a strict superset of the VARCHAR overload. |
| `ST_3DTryFromWKB` | `ST_3DTryFromWKB(wkb BLOB)` | Same as `ST_3DFromWKB`, but returns `NULL` on failure. |
| `ST_3DTryFromWKB` | `ST_3DTryFromWKB(wkb BLOB, geometry_properties VARCHAR)` | Same as above with metadata-aware import. |
| `ST_3DTryFromWKB` | `ST_3DTryFromWKB(wkb BLOB, geometry_properties STRUCT)` | STRUCT-metadata import, returning `NULL` on failure. |
| `ST_3DAsWKB` | `ST_3DAsWKB(solid SOLID_3D)` | Export the canonicalized solid to supported WKB. |

**Experimental (`arrow-native-type` branch), [§8.3](#83-arrow-native-import):**

| Function | Signature | Behavior |
| --- | --- | --- |
| `ST_3DFromArrowNative` | `ST_3DFromArrowNative(boundaries, vertices, geometry_properties VARCHAR \| STRUCT(...)) → SOLID_3D` | Ingest nested `LIST`/`STRUCT` boundaries + a vertex pool for `Solid`/`MultiSolid`/`CompositeSolid` rows (`geometry_properties.type` checked, not the physical shape — see below); raises if `.type` is not solid-family. Two `geometry_properties` overloads, mirroring `ST_3DFromWKB`. |
| `ST_3DTryFromArrowNative` | `ST_3DTryFromArrowNative(boundaries, vertices, geometry_properties VARCHAR \| STRUCT(...)) → SOLID_3D` or `NULL` | Same as above, returning `NULL` on failure (including a family mismatch). |
| `ST_Geom3DFromArrowNative` | `ST_Geom3DFromArrowNative(boundaries, vertices, geometry_properties VARCHAR \| STRUCT(...)) → GEOM_3D` | Same ingestion for `MultiSurface`/`CompositeSurface` rows (`boundaries` padded to solid-count 1 / shell-count 1); raises if `.type` is not surface-family. |
| `ST_Geom3DTryFromArrowNative` | `ST_Geom3DTryFromArrowNative(boundaries, vertices, geometry_properties VARCHAR \| STRUCT(...)) → GEOM_3D` or `NULL` | Same as above, returning `NULL` on failure (family mismatch or non-padded input). |

**`geometry_properties` is required, not optional, and its `.type` field is load-bearing —
consumers dispatch on it, never on the physical shape.** A single `geometry_lod*` column may
legitimately mix `Solid`-family and (padded) `MultiSurface`-family rows sharing the *identical*
physical shape (solid-count 1, shell-count 1 is indistinguishable from a real single-shell
`Solid` by shape alone) — this is exactly why an earlier draft of this API that dropped
`geometry_properties` and dispatched by *which* of these four functions a query calls was wrong:
it silently misinterpreted whichever family didn't match the caller's chosen function for an
entire column, rather than rejecting only the mismatched rows. `geometry_properties` is JSON text
(`VARCHAR`), parsed via the same `ParseGeometryProperties` the WKB path already uses ([§8.2](#82-metadata-aware-import))
— the arrow-native path keeps this identical to WKB rather than growing the [§5.1](#51-constructor-and-export-functions)
STRUCT overload's shape, so the shared parsing step stays byte-for-byte the same between the two
ingestion paths.

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

The second argument may be given two ways, and they are equivalent:

- **JSON text** in a `VARCHAR`, in the CityParquet **spec §8** `geometry_properties`
  form emitted by `duckdb-cityjson`.
- A **`STRUCT`**, read directly out of a CityParquet `geometry_properties_lod*`
  column (spec §8: `STRUCT("type" VARCHAR, surfaces VARCHAR, face_semantics
  INTEGER[], shells INTEGER[][])`) — no `to_json(...)` round-trip. The overload is
  registered as `(BLOB, ANY)`; its bind requires the argument to be a `STRUCT`
  carrying a `shells` field (a struct without one raises a `BinderException`
  naming the field), locates `shells` / `type` by name (case-insensitive,
  tolerating field order and extra fields), and normalises their element types
  (`type` → `VARCHAR`, `shells` → `HUGEINT[][]`) via a bind-time struct cast so
  any standard integer producer type is accepted. Each face count is then
  range-checked (it must fit in `uint32`) in the executor rather than in the
  cast, so `ST_3DTryFromWKB` returns `NULL` on an out-of-range count instead of
  raising. A `VARCHAR`, JSON-alias, or `NULL` metadata argument is routed back to
  the JSON path unchanged, so the STRUCT overload is a strict superset of the
  `VARCHAR` one.

cityparquet-rs's STRUCT nests `shells` as `List<List<Int32>>` unconditionally
(one entry per solid, even for a single `Solid`), which the import layer accepts
as equivalent to the flat JSON form (see below).

The only field the import layer consumes for shell grouping is:

- **`shells`** — per-shell emitted-face counts. A flat array is one solid
  (`[12]`, `[12, 4]`); a nested array is one per-shell-count array per solid
  — either a single-solid `Solid` wrapped once (`[[12]]`, `[[12, 4]]`, as a
  CityParquet `geometry_properties_lod*` STRUCT serializes it) or one array
  per solid for a `MultiSolid`/`CompositeSolid` (`[[12], [8, 4]]`). This
  recovers the shell partition the WKB `PolyhedralSurface Z` flattens away. A
  `0` entry is a fully-dropped shell (spec §8) and creates no shell.

`type` (a CityJSON string) is read but informational; all other keys
(`surfaces`, `face_semantics`, `lod`, …) are ignored for grouping. A non-string
`type` from a pre-spec producer is tolerated.

If metadata conflicts with WKB (a `shells` solid count that does not match the
WKB member count, or a per-solid face-count sum that does not match its member's
face count):

- `ST_3DFromWKB` raises
- `ST_3DTryFromWKB` returns `NULL`

### 8.2.1 CityJSON shell-grouping support

Driven by the spec §8 `shells` key, shell grouping is supported uniformly:

- `Solid` with plain WKB: one-shell solid
- `Solid` with `shells`: recovered shell grouping (including interior shells)
- `MultiSolid`/`CompositeSolid` with plain WKB: a collection of one-shell solids
- `MultiSolid`/`CompositeSolid` with nested `shells`: full per-solid shell
  grouping, including per-solid interior shells (one per-shell-count array per
  WKB member)

This limitation is explicit and must not be hidden behind silent inference.

### 8.3 Arrow-Native Import

**Experimental (`arrow-native-type` branch)** — see the design doc in the parent workspace repo
(`docs/superpowers/specs/2026-07-25-arrow-native-geometry-design.md`) for the cross-repo-agreed
shape this must match exactly.

`ST_3DFromArrowNative`/`ST_3DTryFromArrowNative`/`ST_Geom3DFromArrowNative`/
`ST_Geom3DTryFromArrowNative` consume the nested `LIST`/`STRUCT` columns
`cityparquet-rs`/`duckdb-cityjson` write directly, bypassing WKB entirely:

- **`boundaries`** — a 5-level nested `LIST`: `solid -> shell -> face -> ring -> index`
  (`LIST<LIST<LIST<LIST<LIST<INTEGER>>>>>>`).
- **`vertices`** — a flat pool, `LIST<STRUCT<x, y, z DOUBLE>>`, referenced by index from the
  boundaries' innermost ring-index lists.

**The "shell" nesting level does not carry real interior-shell structure — confirmed by reading
the actual producer.** An earlier revision of this document claimed "every level is a real list,
even where WKB would flatten it away... shells are never lost." That is false for the `Solid`
family: `cityparquet-rs`'s `arrow_geom_write.rs` (`push_padded_solid`) always pads every solid to
exactly **one** physical shell, flattening any real interior shells into a single face list —
exactly as the WKB path flattens them into one `PolyhedralSurface`. The real per-solid shell
partition lives only in `geometry_properties.shells`, recovered the same way the WKB path already
recovers it ([§8.2.1](#821-cityjson-shell-grouping-support)). This is why `geometry_properties` is
a *required* argument on all four functions, not an optional dispatch hint (next paragraphs) — see
"Shells regrouping" below for the mechanism.

**Distinct-index compaction is the producer's responsibility, not this extension's.** The writer
(`cityparquet-rs`/`duckdb-cityjson`) is expected to emit a vertex pool with duplicate coordinates
already compacted to a single index — that compaction is what makes the encoding worth using over
WKB in the first place. `duckdb-3d` only *consumes* this column shape; it does not perform that
compaction itself. It does, however, defensively re-deduplicate by coordinate equality on the
`SOLID_3D` path (not the `GEOM_3D` path — see below) before running `ValidateSolidModel`, because
this is a public SQL-callable ingestion boundary: a writer bug that leaves geometrically-identical
vertices at distinct pool indices must not silently misreport an open/non-manifold edge (edges are
matched by vertex *index*, not by coordinate, downstream).

**Padding-dimension convention.** `MultiSurface`/`CompositeSurface` values share the exact same
physical `boundaries`/`vertices` shape as a `Solid` — encoded as a solid-count-1, shell-count-1
"padded" value, carrying no solid/shell/cavity meaning whatsoever. This is precisely why physical
shape cannot drive dispatch (next paragraph): a real single-shell `Solid` and a padded
`MultiSurface` are shape-identical by construction.

**Dispatch on `geometry_properties.type`, never on physical shape — this is a hard invariant, not
a convenience.** All four functions take `(boundaries, vertices, geometry_properties)`, with
**two overloads for `geometry_properties`**, mirroring [§5.1](#51-constructor-and-export-functions)'s
WKB overloads exactly: JSON text (`VARCHAR`), parsed via the same `ParseGeometryProperties` the WKB
path already uses; and a native `STRUCT("type" VARCHAR, surfaces JSON, face_semantics INTEGER[],
shells INTEGER[][])`, binding directly against `cityparquet-rs`'s real `geometry_properties_lod*`
column (confirmed: the same `STRUCT` type it already uses for WKB rows) without an explicit
`to_json()` cast. Each function checks `.type` per row before doing anything else:
`ST_3DFromArrowNative`/`ST_3DTryFromArrowNative` accept `Solid`/`MultiSolid`/`CompositeSolid`;
`ST_Geom3DFromArrowNative`/`ST_Geom3DTryFromArrowNative` accept `MultiSurface`/`CompositeSurface`. A
family mismatch raises (or returns `NULL` in `TRY` mode) even when the physical shape alone would
have parsed as a valid value of the *other* family — the padding-dimension assertion inside
`BuildGeomModelFromArrowNative` still runs too, as a defensive secondary check for a producer that
mislabels `.type`, but the primary dispatch is always the metadata field.

**Shells regrouping (`SolidModel` path only).** Because the physical "shell" level is always
padded to one entry per solid (previous paragraph), `BuildSolidModelFromArrowNative` has a
metadata-aware overload — `BuildSolidModelFromArrowNative(boundaries, vertices, metadata)` — that
regroups the flattened per-solid face range into real shells using `metadata.shells`, mirroring
`model_builder.cpp`'s `BuildSolidModel(surfaces, metadata)` exactly: a 64-bit running sum (so a
crafted count near `UINT32_MAX` cannot wrap into a spurious match), a `0` entry is a fully-dropped
shell contributing no shell entry, and at least one non-empty shell is required per solid. Only
`shell_face_offsets`/`solid_shell_offsets` are rederived — `face_ring_offsets`/
`ring_vertex_offsets`/`ring_vertex_indices` are copied through unchanged, since regrouping only
reinterprets which shell boundary a face falls under, never reorders faces. Skipping this
regrouping (an earlier revision of this branch did) silently merges every interior shell into the
exterior, so `CheckInteriorShellWinding` ([§9.3](#93-orientation-consistency)) never runs and a
same-wound (invalid) cavity is accepted with its volume wrongly added instead of subtracted. If
`metadata.shells` is empty, this overload delegates unchanged to the 2-arg overload (matches
`BuildSolidModel(surfaces)` with no metadata: whatever shell grouping the boundaries already have
is used as-is). `GeomModel`/surface types have no shells concept, so no equivalent exists on that
path.

A single `geometry_lod*` column may legitimately mix `Solid`-family and (padded) surface-family
rows sharing the identical physical shape — an earlier draft of this API dropped
`geometry_properties` and dispatched by *which* of these four functions a query calls instead
(mirroring how a SQL author picks `ST_3DFromWKB` vs `ST_Geom3DFromWKB` for a whole WKB column).
That draft was wrong for arrow-native specifically: a WKB column's bytes are genuinely
self-describing per row (the WKB type code says what a row is), so whole-column dispatch happens
to work there; arrow-native's physical shape is *not* self-describing between `Solid` and a padded
`MultiSurface`, so the same shortcut silently misinterprets whichever family didn't match the
caller's chosen function, for an entire column, rather than correctly rejecting only the
mismatched rows.

**Kernel stays format-agnostic.** The nested-`Vector` traversal is entirely a SQL-layer concern
(`three_d_extension.cpp`'s `ExtractArrowNativeBoundaries`/`ExtractArrowNativeVertices`), which
flattens the DuckDB column data into a plain-C++ CSR form (`ArrowNativeBoundaries`,
`src/include/kernel/arrow_native_import.hpp`) before handing off to
`BuildSolidModelFromArrowNative`/`BuildGeomModelFromArrowNative` — functions that never see a
DuckDB `Vector`, keeping `src/kernel/` DuckDB-free exactly as `ParseWKB` does for the WKB path.
`GeomModel`'s asymmetry from `SolidModel` (§6) carries through here too: `SolidModel` stays
index-based (indices copied/remapped, not dereferenced), while `GeomModel` dereferences and
expands indices into inline coordinates, since `GeomModel` is never index-based to begin with.

## 9. Validation Rules

Validation is part of import and report generation.

### 9.1 Closedness

A shell is closed when every undirected edge is referenced exactly twice with opposing local orientation.

### 9.2 Manifoldness

A shell is manifold when no undirected edge belongs to more than two incident faces and local connectivity does not create branching topology.

### 9.3 Orientation Consistency

Orientation checks ensure:

- each shell has consistent face winding (per-shell edge cancellation)
- interior shells, when present, are oriented opposite to the exterior shell —
  **enforced** by `CheckInteriorShellWinding` (§10.2.1): shell 0 is the exterior,
  and each interior shell must be wound opposite it and be smaller in |signed
  volume| (a larger shell cannot be contained). A violation clears `is_oriented`
  / `is_valid`, so `ST_3DVolume` refuses rather than returning a wrong total. The
  check is relative-only (the exterior's absolute outward orientation and true
  point-in-polyhedron containment are out of scope) and a no-op for single-shell
  solids.

v1 does not silently correct orientation.

### 9.4 Degenerate Faces

Faces are degenerate if:

- fewer than 3 distinct vertices remain after normalization
- triangulation fails because the face is not a valid polygon in 3D
- computed area is zero within tolerance

### 9.5 Tolerance Policy

v1 uses a small floating-point epsilon for repeated-point and near-zero-area checks. The epsilon is an implementation constant and must be documented in code when introduced.

#### 9.5.1 Differential Oracle (PostGIS/SFCGAL)

The extension's measurement math is cross-checked against **PostGIS + SFCGAL** as an independent reference oracle. PostGIS is **never** a build, runtime, or CI dependency: it runs offline, dev-time only, to produce frozen golden values that the normal test suite compares against.

- **Harness.** `scripts/oracle/gen_golden.py` exports the extension's own geometry as ISO WKB (`ST_3DAsWKB` → `PolyhedralSurface Z`), feeds the *same bytes* to SFCGAL, and freezes the reference values into `test/data/postgis_oracle/golden.csv` (per-geometry: volume, surface area, closedness, 2D convex-hull area) and `golden_pairs.csv` (per-pair: `ST_3DDistance`, `ST_3DMaxDistance`, `ST_3DIntersects`, `ST_3DDWithin`, `ST_3DDFullyWithin`, shortest-line length, closest-point distance). The CI test `test/sql/postgis_oracle.test` re-imports that frozen WKB with `ST_3DFromWKB` / `ST_Geom3DFromWKB` and asserts agreement — `require three_d` only, no PostGIS, no network. Feeding identical bytes to both engines isolates the *math* from ingestion/quantisation differences. See `test/data/postgis_oracle/README.md`.
- **Two-tier numeric tolerance** on `|a − b| ≤ rel·|b| + abs`:
  - *tight* (`rel = 1e-6, abs = 1e-9`) — analytic fixtures with small integer coordinates and exactly-planar faces (unit tetrahedron); agreement is essentially machine-precision.
  - *loose* (`rel = 1e-3, abs = 1e-6`) — the real accepted 3DBAG geometry; conservative headroom for large real-world coordinate magnitudes, where the two engines' summation/triangulation order differs by a few ULPs (the one row SFCGAL currently accepts in fact agrees to ~6e-8, well inside the tight tier).
  - Booleans (closedness) are compared **exactly**.
- **Planarity boundary.** SFCGAL's area/volume require exactly-coplanar polygon faces and *reject* (do not approximate) most real reconstructed roofs; the extension deliberately measures such faces by triangulation, so where SFCGAL rejects, the oracle of record for real-geometry area/volume is 3DBAG's published attributes (`cityjson_delft_remote.test`), not SFCGAL. The 3D distance/relation predicates have no such requirement — they compute on the raw non-planar surfaces, so `golden_pairs.csv` covers real geometry directly.
- **Invalid geometry.** PostGIS is the *wrong* oracle here (it repairs or rejects, the opposite of the extension's "fail clearly, no repair" contract). The extension's own `ST_3DValidationReport` is the oracle of record; SFCGAL rejection is recorded only as corroboration.

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

#### 10.2.1 Interior (Inner) Shell Handling — Mechanism And Rationale

This subsection is the reference for **how a solid with interior shells (cavities) is
measured, and why the implementation is shaped the way it is**. It documents behaviour that
was previously only implicit in the code (`ComputeVolume`, `src/kernel/measurements.cpp`).

**Mechanism.** `ComputeVolume` iterates over *every* shell of a solid and accumulates the
signed tetrahedral volume of each triangle into a single `solid_volume` running total, then
takes the absolute value **once per solid** before summing across solids:

```text
for each solid:
    solid_volume = Σ_shells Σ_faces Σ_triangles  SignedTriangleVolume(tri)   // signed
    total_volume += abs(solid_volume)                                        // abs per solid
total_volume /= 6
```

`SignedTriangleVolume` is `a · (b × c)` — the signed volume of the tetrahedron from the
origin to the triangle. Its sign is determined entirely by the triangle's winding, i.e. by
**face orientation**.

**Why interior shells subtract automatically.** By the orientation contract (§9.3), an
interior shell is wound *opposite* to the exterior shell. Therefore:

- the exterior shell contributes `+V_outer`
- an interior shell contributes `−V_inner`
- their sum is the net material volume `V_outer − V_inner`

No explicit "this shell is a hole" branch is needed: the subtraction falls out of the signed
arithmetic plus the opposite-orientation invariant.

**Why `abs` is applied per solid, after summing all shells — not per shell.** Global winding
direction is ambiguous (a solid may be stored CCW-outward or CW-outward; §9.3 enforces only
*consistency*, not a fixed handedness). Taking `abs` once, after the interior/exterior terms
have already cancelled, collapses that global ambiguity **while preserving** the cavity
subtraction. Taking `abs` per shell would make cavities *add* instead of subtract, and would
be wrong.

**Recognition of inner vs outer is by orientation, not by a stored flag.** The canonical
model does not tag a shell as "interior". Which shell subtracts is decided implicitly by its
signed contribution. The larger, positively-oriented exterior shell dominates; an
oppositely-wound nested shell subtracts. This mirrors how PostGIS/SFCGAL determines
inner/outer for a solid — also by orientation and signed volume — because standard WKB
`PolyhedralSurface` carries no shell-grouping token (see §8, §12).

**Volume is shell-grouping *invariant* for a disjoint cavity.** `ComputeVolume` sums signed
triangle volumes over *all* faces of the solid, so the subtraction is driven entirely by the
interior shell's inward **winding**, not by whether import grouped the faces into separate
shells. A plain WKB `PolyhedralSurface Z` import (which collapses everything into **one shell
per solid**, §5.1.3) of a correctly-wound cavity still produces the *same, correct* volume —
and, because the exterior and cavity surfaces share no edges, still passes the per-edge
closed/manifold checks (they are **not** rejected as non-manifold; each closed component
cancels its own edges independently). What the spec §8 `geometry_properties`
`shells` key (§8.2.1; flat for `Solid`, nested per solid for
`MultiSolid`/`CompositeSolid`) actually buys is correct **introspection**
(`ST_3DNumShells` reports 2 rather than 1) **and** the shell partition the
winding check below needs.

**The interior-opposite-exterior invariant is enforced.** §9.3 requires interior
shells to be wound opposite the exterior. `CheckInteriorShellWinding` computes
each shell's signed volume (origin-translated tetra sum, to survive projected-CRS
coordinates) and requires every interior shell to be opposite-signed to, and
smaller in magnitude than, the exterior (shell 0). A same-wound cavity — whose
volume would **add** (`V_outer + V_inner`) instead of subtract — now clears
`is_oriented`/`is_valid`, so `ST_3DVolume` refuses. This needs the shell
partition, so it fires on the metadata (`shells`) import path, not the plain
one-merged-shell path. The behaviour is pinned by `test/cpp/test_inner_shell.cpp`.
Scope is relative opposition only; the exterior's absolute orientation and true
containment remain out of scope.

**Surface area and footprint are not shell-aware.** `ST_3DSurfaceArea` / `ST_3DArea`
(`ComputeSurfaceArea`) and `ST_3DFootprintArea` (`ComputeFootprintArea`) sum over *all* faces without any
shell logic, so interior-shell (cavity) walls contribute to reported surface area. Only volume
distinguishes shell roles.

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
- `cityjson` supplies sidecar `geometry_properties`, either as JSON text in a `VARCHAR`, or — on
  `duckdb-cityjson`'s experimental `arrow-native-type` branch (commit `d334b26`) — as a native
  `STRUCT("type" VARCHAR, surfaces JSON, face_semantics INTEGER[], shells INTEGER[][])`. The query
  above is unchanged either way: `ST_3DFromWKB`'s three overloads ([§5.1](#51-constructor-and-export-functions))
  resolve on the column's actual runtime type, so no `to_json()` cast is needed for the STRUCT case.
- `duckdb-3d` does not need to know about CityJSON files or tables directly

Future integration work may improve metadata richness, but v1 keeps the extensions decoupled.

**Experimental (`arrow-native-type` branch) — arrow-native equivalent, [§8.3](#83-arrow-native-import):**
when `read_cityjson`/`cityparquet-rs` expose `boundaries`/`vertices` columns directly (rather than
WKB), the WKB byte-parsing step drops out — but `geometry_properties` stays, since `.type` is now
the *only* thing that can distinguish a `Solid` row from a physically-identical padded
`MultiSurface` row in the same column:

```sql
LOAD three_d;

WITH objects AS (
    SELECT id, boundaries, vertices, geometry_properties
    FROM read_cityjson_arrow_native('buildings.city.json', lod => '2.2') -- illustrative; not yet implemented upstream
)
SELECT
    id,
    ST_3DVolume(ST_3DTryFromArrowNative(boundaries, vertices, geometry_properties)) AS volume
FROM objects
WHERE boundaries IS NOT NULL;
```

`ST_3DTryFromArrowNative` is the natural default here (over the raising `ST_3DFromArrowNative`)
precisely because a real `buildings.city.json` column may contain non-solid-family rows
(`MultiSurface`/`CompositeSurface`) that this query isn't asking for — they resolve to `NULL`
rather than aborting the whole scan.

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

## 14. Roadmap Beyond The Current Surface

The v1 baseline (§5) and the class-generic `GEOM_3D` accessor/transform/distance/
serialization surface (§16) are implemented. What remains open is tracked in two places:

- the prioritised, per-function backlog — `should` items not yet built and every `want`
  item — lives in [§16](#16-3d-function-roadmap-postgis-derived);
- three larger design-level workstreams — composite/multi-solid interior shells, moving
  CityJSON-aware interpretation upstream into `duckdb-cityjson`, and CRS/`ST_3DTransform`
  support — are written up in [FUTURE_WORK.md](./FUTURE_WORK.md);
- the deferred workstreams below.

### 14.1 Near-Term

- performance tuning (bbox pre-filters for the distance family, etc.)
- richer metadata-aware import
- improved interoperability with CityJSON multi-shell and multi-solid cases
  ([FUTURE_WORK.md §1](./FUTURE_WORK.md#1-composite--multi-solid-support-with-interior-shells))
- move CityJSON-specific interpretation out of the kernel into `duckdb-cityjson`
  ([FUTURE_WORK.md §2](./FUTURE_WORK.md#2-move-cityjson-aware-interpretation-out-of-duckdb-3d))
- CRS / SRID awareness and `ST_3DTransform`
  ([FUTURE_WORK.md §3](./FUTURE_WORK.md#3-coordinate-reference-system-support-st_3dtransform-srid))

### 14.2 Deferred (backend decision required)

- evaluate an optional CGAL or SFCGAL backend
- 3D boolean operations (union / difference / intersection)
- topology repair workflows
- broader geometry-class coverage

The CGAL/SFCGAL-gated function cluster is specified in
[§16.8](#168-cgal--sfcgal-backend-cluster-all-want-flagged).

### 14.3 Experimental: Arrow-Native Ingestion (`arrow-native-type` branch)

[§8.3](#83-arrow-native-import)'s `ST_3DFromArrowNative`/`ST_3DTryFromArrowNative`/
`ST_Geom3DFromArrowNative`/`ST_Geom3DTryFromArrowNative` are the third leg of a 3-repo experiment
(alongside `cityparquet-rs` and `duckdb-cityjson`), not yet part of the decided v1 surface this
document otherwise describes. Status, the cross-repo schema-parity requirement, and the overall
testing plan live in the parent workspace repo's own design doc:
`docs/superpowers/specs/2026-07-25-arrow-native-geometry-design.md` (draft, under evaluation — its
own status is the authoritative source, not a copy of it here).

## 15. Contribution Invariants

These invariants hold for every change to the extension. A change is complete only when:

- the SQL contract matches this design, or the design is updated intentionally in the same change
- tests exist before implementation changes (the TDD workflow of §13)
- docs and tests are updated together
- no silent topology repair has been introduced
- binary format changes are versioned explicitly (§7.5)

## 16. 3D Function Roadmap (PostGIS-derived)

This section is a prioritised, implementable specification of the 3D functions
`duckdb-3d` will support beyond v1, derived from the useful subset of PostGIS's 3D
function set (reference:
<https://postgis.net/docs/PostGIS_Special_Functions_Index.html>). Each `must`/`should`
function carries an I/O specification detailed enough to implement one at a time under the
TDD workflow of §13.

Each function below is tagged with its status. Functions marked **✅ implemented** exist in
the current build and are catalogued with their kernel location in
[§16.9](#169-implemented-functions-postgis-analogues); everything else is roadmap. Everything
in §1–§15 remains the architectural source of truth; where this roadmap and those sections
ever disagree, §1–§15 win and this section is corrected.

**What is implemented today.** Beyond the v1 baseline (§5), the extension ships the
general-geometry type `GEOM_3D` (§16.2) and the full `must`/`should` accessor, measurement,
distance, transform, construction, and serialization surface of §16.3–§16.7. Concretely, the
implemented set includes: accessors `ST_NDims`, `ST_3DHasZ`, `ST_3DZMin`, `ST_3DZMax`, `ST_3DX`,
`ST_3DY`, `ST_3DZ`, `ST_CoordDim`, `ST_3DGeometryType`, `ST_3DDimension`, `ST_3DNumGeometries`;
measurement `ST_3DFootprintArea`, `ST_3DLength`, `ST_3DPerimeter`, `ST_3DArea`; the distance family
`ST_3DDistance`, `ST_3DDWithin`, `ST_3DMaxDistance`, `ST_3DDFullyWithin`, `ST_3DIntersects`,
`ST_3DClosestPoint`, `ST_3DShortestLine`; transforms/construction `ST_3DTranslate`, `ST_3DScale`,
`ST_3DRotateX/Y/Z`, `ST_Force3D`, `ST_3DExtrude`, `ST_MakeSolid`, `ST_3DCentroid`,
`ST_3DConvexHull`, `ST_IsPlanar`; and serialization `ST_3DAsText`, `ST_3DAsGeoJSON`, `ST_3DAsBinary`.
The `GEOM_3D` WKB parser covers `Point Z`, `LineString Z`, `MultiPoint Z`,
`MultiLineString Z`, `Polygon Z`, `MultiPolygon Z`, and `PolyhedralSurface Z`. Kernel sources:
`geom_distance.cpp`, `geom_construct.cpp`, `geom_analysis.cpp`, `geom_serialize.cpp`,
`measurements.cpp`. The full table with PostGIS analogues is
[§16.9](#169-implemented-functions-postgis-analogues).

**What remains on the roadmap** is the `want` tier of §16.3–§16.7 (e.g. `ST_HasM`, `ST_M`,
`ST_Zmflag`, `ST_3DLongestLine`, `ST_Affine`, `ST_FlipCoordinates`, `ST_SwapOrdinates`,
`ST_PointOnSurface`, `ST_Boundary`, `ST_AsX3D`, `ST_AsGML`, `ST_AsKML`) and the entire
CGAL/SFCGAL cluster of §16.8.

On naming: we use `ST_3D*` names (see [§3.1](#31-naming-note) — the namespace that lets
`three_d` coexist with `spatial`), but per §2.2 **full PostGIS parity is a non-goal** — this
is a curated subset chosen for 3D city-model workflows.

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
  `SOLID_3D` via `ST_MakeSolid` (§16.6).
- The v1 `D3DS` payload (§7) remains solid-specific. A sibling **`D3DG` payload**
  (`src/kernel/geom_payload.cpp`) carries lower-dimensional geometries — a versioned
  change under the §7.5 compatibility rules. This is what backs the class-generic
  accessor and transform functions below.

Functions in this section declare their accepted type(s): `GEOM_3D` for class-generic
operations, `SOLID_3D` for solid-only operations, or both. Null / error semantics are
inherited from §4.2 (any `NULL` argument → `NULL`; non-`TRY` raise; `TRY` returns `NULL`).

### 16.3 Accessors And Properties

Class-generic; operate on `GEOM_3D` (and `SOLID_3D` where a bounding box suffices).

| Function | Signature (input → output) | Priority | Backend | Notes / preconditions |
| --- | --- | --- | --- | --- |
| `ST_NDims` | `(geom GEOM_3D) → INTEGER` | must | kernel | ✅ implemented on `SOLID_3D` and `GEOM_3D`. Coordinate dimension (3 for XYZ). |
| `ST_3DHasZ` | `(geom GEOM_3D) → BOOLEAN` | must | kernel | ✅ implemented on `SOLID_3D` and `GEOM_3D`. True if geometry carries Z (always true in v1; future-proofs XY inputs). |
| `ST_3DZ` | `(point GEOM_3D) → DOUBLE` | must | kernel | ✅ implemented on `GEOM_3D`. Z of a Point; `NULL` if empty. Raises if not a Point. |
| `ST_3DZMax` | `(geom GEOM_3D) → DOUBLE` | must | kernel | ✅ implemented on `SOLID_3D` and `GEOM_3D`. Max Z of bbox. Building roof elevation. |
| `ST_3DZMin` | `(geom GEOM_3D) → DOUBLE` | must | kernel | ✅ implemented on `SOLID_3D` and `GEOM_3D`. Min Z of bbox. `ZMax − ZMin` = building height. |
| `ST_3DX` | `(point GEOM_3D) → DOUBLE` | should | kernel | ✅ implemented on `GEOM_3D`. X of a Point. Raises if not a Point. |
| `ST_3DY` | `(point GEOM_3D) → DOUBLE` | should | kernel | ✅ implemented on `GEOM_3D`. Y of a Point. Raises if not a Point. |
| `ST_CoordDim` | `(geom GEOM_3D) → INTEGER` | should | kernel | ✅ implemented on `GEOM_3D`. Coordinate dimension (alias-like to `ST_NDims` in v1). |
| `ST_3DGeometryType` | `(geom GEOM_3D) → VARCHAR` | should | kernel | ✅ implemented on `GEOM_3D`. e.g. `ST_PolyhedralSurface`, `ST_Polygon`. |
| `ST_3DDimension` | `(geom GEOM_3D) → INTEGER` | should | kernel | ✅ implemented on `GEOM_3D`. Topological dim: 0 point, 1 line, 2 surface, 3 solid. |
| `ST_3DNumGeometries` | `(geom GEOM_3D) → INTEGER` | should | kernel | ✅ implemented on `GEOM_3D`. Member count of a collection/multi geometry. |
| `ST_HasM` | `(geom GEOM_3D) → BOOLEAN` | want | kernel | M dimension not used by city models. |
| `ST_M` | `(point GEOM_3D) → DOUBLE` | want | kernel | M of a Point. |
| `ST_Zmflag` | `(geom GEOM_3D) → SMALLINT` | want | kernel | ZM dimensionality code. |

### 16.4 Measurement

| Function | Signature (input → output) | Priority | Backend | Notes / preconditions |
| --- | --- | --- | --- | --- |
| `ST_3DFootprintArea` | `(geom GEOM_3D) → DOUBLE` | must | kernel | ✅ implemented on **both** `SOLID_3D` (footprint, §10-area) and `GEOM_3D` (XY-projected polygon area, single-sided; enables `ST_3DFootprintArea(ST_3DConvexHull(g))`). **2D footprint area** = area of XY projection. Key building metric. |
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
| `ST_3DClosestPoint` | `(g1 GEOM_3D, g2 GEOM_3D) → GEOM_3D` | should | kernel | ✅ implemented. 3D point on `g1` closest to `g2`; reuses `Geom3DClosestPoints`. |
| `ST_3DShortestLine` | `(g1 GEOM_3D, g2 GEOM_3D) → GEOM_3D` | should | kernel | ✅ implemented. 3D shortest LineString between geometries; reuses `Geom3DClosestPoints`. |
| `ST_3DLongestLine` | `(g1 GEOM_3D, g2 GEOM_3D) → GEOM_3D` | want | kernel | 3D longest line. |

### 16.6 Transformations And Construction

| Function | Signature (input → output) | Priority | Backend | Notes / preconditions |
| --- | --- | --- | --- | --- |
| `ST_3DTranslate` | `(geom GEOM_3D, dx DOUBLE, dy DOUBLE, dz DOUBLE) → GEOM_3D` | must | kernel | ✅ implemented on `SOLID_3D` and `GEOM_3D`. Placement / georeferencing. Topology preserved. |
| `ST_3DScale` | `(geom GEOM_3D, sx DOUBLE, sy DOUBLE, sz DOUBLE) → GEOM_3D` | should | kernel | ✅ implemented on `SOLID_3D` and `GEOM_3D`. Scale about origin. |
| `ST_3DRotateX` | `(geom GEOM_3D, radians DOUBLE) → GEOM_3D` | should | kernel | ✅ implemented on `SOLID_3D` and `GEOM_3D`. Rotate about X axis. |
| `ST_3DRotateY` | `(geom GEOM_3D, radians DOUBLE) → GEOM_3D` | should | kernel | ✅ implemented on `SOLID_3D` and `GEOM_3D`. Rotate about Y axis. |
| `ST_3DRotateZ` | `(geom GEOM_3D, radians DOUBLE) → GEOM_3D` | should | kernel | ✅ implemented on `SOLID_3D` and `GEOM_3D`. Rotate about Z axis. |
| `ST_3DTransform` | `(geom, source_srid INT, target_srid INT) → same type` and `(geom, source_crs VARCHAR, target_crs VARCHAR) → same type` | should | **PROJ** | ✅ implemented on `SOLID_3D` and `GEOM_3D`. **Horizontal (2D) only**: reprojects X/Y, preserves Z (no vertical datum), matching PostGIS default. Axis order normalised to easting/northing (lon/lat). bbox recomputed; solids re-validated. The only function with an external backend (PROJ); confined to `src/kernel/crs_transform.cpp`. Remaining CRS work (stored SRID, vertical datum, `proj.db` bundling) in [FUTURE_WORK.md §3](./FUTURE_WORK.md#3-coordinate-reference-system-support-st_3dtransform-srid). |
| `ST_Force3D` / `ST_Force3DZ` | `(geom GEOM_3D) → GEOM_3D` | should | kernel | ✅ implemented. Identity on GEOM_3D (already XYZ); future 2D inputs would gain Z=0. |
| `ST_3DExtrude` | `(polygon GEOM_3D, height DOUBLE) → SOLID_3D` | should | kernel | ✅ implemented. **Vertical** prism extrusion (footprint → LoD1 box); footprint normalised to CCW, result validated closed+oriented. Returns the `SOLID_3D` alias type. |
| `ST_MakeSolid` | `(geom GEOM_3D) → SOLID_3D` | should | kernel | ✅ implemented. Cast a closed/oriented/manifold `PolyhedralSurface` to a solid; raises if not solid-eligible (no repair). Returns the `SOLID_3D` alias type. |
| `ST_3DCentroid` | `(geom GEOM_3D) → GEOM_3D` | should | kernel | ✅ implemented. Dimension-aware centroid: vertex average (points), length-weighted (lines), area-weighted (surfaces). |
| `ST_3DConvexHull` | `(geom GEOM_3D) → GEOM_3D` | should | kernel | ✅ implemented. **2D** monotone-chain convex hull (XY); returns Polygon Z / LineString Z / Point Z at min Z. |
| `ST_IsPlanar` | `(geom GEOM_3D) → BOOLEAN` | should | kernel | ✅ implemented. Whether all faces/rings are planar within tolerance (§9.5). City-model QA. |
| `ST_Affine` | `(geom GEOM_3D, a..l DOUBLE ×12) → GEOM_3D` | want | kernel | General 3D affine; rarely called directly. |
| `ST_FlipCoordinates` | `(geom GEOM_3D) → GEOM_3D` | want | kernel | Swap X/Y. |
| `ST_SwapOrdinates` | `(geom GEOM_3D, spec VARCHAR) → GEOM_3D` | want | kernel | Swap named ordinates. |
| `ST_PointOnSurface` | `(geom GEOM_3D) → GEOM_3D` | want | kernel | Guaranteed on-surface point. |
| `ST_Boundary` | `(geom GEOM_3D) → GEOM_3D` | want | kernel | Topological boundary. |

### 16.7 Serialization / Output

| Function | Signature (input → output) | Priority | Backend | Notes / preconditions |
| --- | --- | --- | --- | --- |
| `ST_3DAsText` | `(geom GEOM_3D) → VARCHAR` | should | kernel | ✅ implemented. ISO WKT (with Z). Inspection / debugging. |
| `ST_3DAsGeoJSON` | `(geom GEOM_3D) → VARCHAR` | should | kernel | ✅ implemented. GeoJSON; PolyhedralSurface emitted as MultiPolygon. |
| `ST_3DAsBinary` | `(geom GEOM_3D) → BLOB` | should | kernel | ✅ implemented. OGC/ISO WKB (complements solid-only `ST_3DAsWKB`, §5.1). |
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

#### 16.9.2 Class-generic and roadmap functions (implemented)

Implemented on `SOLID_3D` and, where class-generic, on `GEOM_3D`. Kernel helpers:
`ComputeFootprintArea`, `ComputePerimeter` (`src/kernel/measurements.cpp`); the
primitive-distance kernel `Geom3DDistance`, its point/segment/triangle primitives,
and the closest-point-pair kernel `Geom3DClosestPoints`
(`src/kernel/geom_distance.cpp`); the analysis helpers `Geom3DIsPlanar` and
`Geom3DCentroid` (`src/kernel/geom_analysis.cpp`); the serialization helpers
`Geom3DAsText`, `Geom3DAsGeoJSON`, and `Geom3DAsBinary`
(`src/kernel/geom_serialize.cpp`).

| `duckdb-3d` function | Signature | Priority | PostGIS analogue |
| --- | --- | --- | --- |
| `ST_NDims` | `(solid SOLID_3D \| geom GEOM_3D) → INTEGER` | must | `ST_NDims` |
| `ST_3DHasZ` | `(solid SOLID_3D \| geom GEOM_3D) → BOOLEAN` | must | `ST_HasZ` |
| `ST_3DZMin` | `(solid SOLID_3D \| geom GEOM_3D) → DOUBLE` | must | `ST_ZMin` |
| `ST_3DZMax` | `(solid SOLID_3D \| geom GEOM_3D) → DOUBLE` | must | `ST_ZMax` |
| `ST_3DFootprintArea` | `(solid SOLID_3D) → DOUBLE` | must | `ST_Area` (footprint) |
| `ST_3DPerimeter` | `(solid SOLID_3D) → DOUBLE` | should | `ST_3DPerimeter` |
| `ST_3DArea` | `(solid SOLID_3D) → DOUBLE` | should | `ST_3DArea` / `CG_3DArea` |
| `ST_3DTranslate` | `(solid SOLID_3D \| geom GEOM_3D, dx, dy, dz DOUBLE) → same type` | must | `ST_Translate` |
| `ST_3DScale` | `(solid SOLID_3D \| geom GEOM_3D, sx, sy, sz DOUBLE) → same type` | should | `ST_Scale` |
| `ST_3DRotateX` | `(solid SOLID_3D \| geom GEOM_3D, radians DOUBLE) → same type` | should | `ST_RotateX` |
| `ST_3DRotateY` | `(solid SOLID_3D \| geom GEOM_3D, radians DOUBLE) → same type` | should | `ST_RotateY` |
| `ST_3DRotateZ` | `(solid SOLID_3D \| geom GEOM_3D, radians DOUBLE) → same type` | should | `ST_RotateZ` |
| `ST_3DTransform` | `(solid SOLID_3D \| geom GEOM_3D, source, target [INT\|VARCHAR]) → same type` | should | `ST_Transform` (2D only) |
| `ST_3DZ` | `(point GEOM_3D) → DOUBLE` | must | `ST_Z` |
| `ST_3DX` | `(point GEOM_3D) → DOUBLE` | should | `ST_X` |
| `ST_3DY` | `(point GEOM_3D) → DOUBLE` | should | `ST_Y` |
| `ST_CoordDim` | `(geom GEOM_3D) → INTEGER` | should | `ST_CoordDim` |
| `ST_3DGeometryType` | `(geom GEOM_3D) → VARCHAR` | should | `ST_GeometryType` |
| `ST_3DDimension` | `(geom GEOM_3D) → INTEGER` | should | `ST_Dimension` |
| `ST_3DNumGeometries` | `(geom GEOM_3D) → INTEGER` | should | `ST_NumGeometries` |
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
| `ST_Force3D` | `(geom GEOM_3D) → GEOM_3D` | should | `ST_Force3D` |
| `ST_3DConvexHull` | `(geom GEOM_3D) → GEOM_3D` | should | `ST_ConvexHull` |
| `ST_3DClosestPoint` | `(g1 GEOM_3D, g2 GEOM_3D) → GEOM_3D` | should | `ST_3DClosestPoint` |
| `ST_3DShortestLine` | `(g1 GEOM_3D, g2 GEOM_3D) → GEOM_3D` | should | `ST_3DShortestLine` |
| `ST_3DAsText` | `(geom GEOM_3D) → VARCHAR` | should | `ST_AsText` |
| `ST_3DAsGeoJSON` | `(geom GEOM_3D) → VARCHAR` | should | `ST_AsGeoJSON` |
| `ST_3DAsBinary` | `(geom GEOM_3D) → BLOB` | should | `ST_AsBinary` |

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

**`ST_3DHasZ(geom GEOM_3D) → BOOLEAN`**
- True when the geometry carries a Z ordinate.
- Tests: SQL — XYZ geometry → true.

**`ST_3DZ(point GEOM_3D) → DOUBLE`**
- Z ordinate of a single Point. `NULL` for empty point. Raises if the geometry is not a
  Point.
- Tests: unit — point (1,2,3) → 3.0; SQL — non-point input raises.

**`ST_3DZMax(geom GEOM_3D) → DOUBLE`** / **`ST_3DZMin(geom GEOM_3D) → DOUBLE`**
- Max/min Z from the cached bounding box (reuse the bbox computed at import, §6.2). For
  `SOLID_3D`, equals `ST_3DBounds(...).max_z` / `.min_z`.
- `ZMax − ZMin` is the canonical building-height expression; document this in the example.
- Tests: unit — known bbox; SQL — equality with `ST_3DBounds` fields.

#### 16.10.2 Measurement — `must`

**`ST_3DFootprintArea(geom GEOM_3D) → DOUBLE`**
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
- **GEOM_3D overload:** the same `ST_3DFootprintArea` also accepts a `GEOM_3D` value (dispatched
  by payload magic, like `ST_3DZMin`/`ST_3DZMax`; an untyped `NULL` is therefore ambiguous
  and must be cast, exactly as for `ST_3DZMin`). Semantics by class:
  - **Polygon / MultiPolygon** — single-sided, **no halving**: the XY-projected area of
    each exterior ring minus its interior (hole) rings, summed over parts. This is the
    `ST_3DConvexHull` case: `ST_3DFootprintArea(ST_3DConvexHull(g))` gives the hull area, matching
    PostGIS `ST_Area(ST_ConvexHull(ST_Points(g)))` in the differential harness
    (§9.5.1) — GEOS's `ST_ConvexHull` rejects a `PolyhedralSurface`, so it is fed
    the vertex set via `ST_Points`.
  - **PolyhedralSurface** — a two-sided shell, so **halved** (`0.5·Σ|projected face|`),
    consistent with the `SOLID_3D` footprint above; exact for closed vertically-simple
    shells, approximate otherwise.
  - **Points / lines / vertical faces** — 0.

  Implemented in `Geom3DFootprintArea` (`src/kernel/geom_analysis.cpp`), which guards
  against malformed (unbounded) CSR offsets rather than reading past the vertex array.

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

**`ST_3DTranslate(geom GEOM_3D, dx DOUBLE, dy DOUBLE, dz DOUBLE) → GEOM_3D`**
- Adds `(dx,dy,dz)` to every vertex; topology, shell/face structure, and validation flags
  are preserved (translation cannot change closedness/manifoldness/orientation); bbox is
  shifted. Re-emit payload without recomputing validation.
- Tests: unit — translate then inverse-translate equals original within epsilon; bbox
  shifted correctly; validation flags unchanged.

#### 16.10.5 `should` Functions

For `should` functions the I/O contract is the table row in §16.3–§16.7 plus these notes:

- **`ST_3DX` / `ST_3DY`** — mirror `ST_3DZ`; Point-only, raise otherwise.
- **`ST_CoordDim`, `ST_3DGeometryType`, `ST_3DDimension`, `ST_3DNumGeometries`** — pure header
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
- **`ST_3DScale` / `ST_3DRotateX/Y/Z` / `ST_Force3D`** — per-vertex affine maps; recompute
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
- **`ST_3DConvexHull`** — 2D monotone-chain hull over XY-projected vertices; returns a
  `Polygon Z` at the min Z (document the Z convention).
- **`ST_IsPlanar`** — per-face: max point-to-best-fit-plane distance ≤ tolerance (§9.5).
- **`ST_3DAsText` / `ST_3DAsGeoJSON` / `ST_3DAsBinary`** — serialise from the canonical model;
  `ST_3DAsBinary` reuses the existing WKB export path (`src/kernel/wkb_export.cpp`).

### 16.11 Remaining Roadmap Sequencing (suggested)

Steps 1–5 below — the `GEOM_3D` type plus the full `must`/`should` accessor, measurement,
distance, transform, construction, and serialization surface — are **implemented** (§16.9).
What remains:

6. **`want`** items (§16.3–§16.7) as demand appears; the **CGAL/SFCGAL cluster** (§16.8) only
   after the backend decision in §14 is made.

Every step follows red-green-refactor (§13): failing `test/cpp/` math test and `test/sql/`
contract test first, then implementation and registration, then update of §5 and this
section together.
