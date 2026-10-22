# `duckdb-3d` Function Reference

Every SQL function the `three_d` extension registers, with signatures, return types, error
behaviour, and a runnable example.

Every example on this page was **executed against the real 3DBAG Delft dataset** and the
output pasted verbatim. If an example here disagrees with the build, the example is wrong —
please open an issue.

- For the architecture and the *why*, see [DESIGN_DOC.md](./DESIGN_DOC.md).
- For a narrative end-to-end walkthrough, see [EXAMPLE.md](./EXAMPLE.md).
- For composing with the `cityjson` extension, see [CITYJSON_INTEROP.md](./CITYJSON_INTEROP.md).

---

## Contents

- [Setup](#setup)
- [The two types](#the-two-types)
- [Conventions](#conventions)
- [Import / construction](#import--construction)
- [Export / serialization](#export--serialization)
- [Introspection / accessors](#introspection--accessors)
- [Faces, shells and parts](#faces-shells-and-parts)
- [Validation](#validation)
- [Measurement](#measurement)
- [Distance / relationships](#distance--relationships)
- [CRS](#crs)
- [Transform / construct](#transform--construct)
- [SQL/MM profile](#sqlmm-profile)
- [Function index](#function-index)
- [Appendix: test fixtures](#appendix-test-fixtures)

---

## Setup

The examples use the [3DBAG](https://3dbag.nl) reconstruction of Delft, published as
CityJSON and CityJSONSeq:

- CityJSONSeq — `https://cityjson.open3d.city/cityjsonseq/delft.city.jsonl`
- CityJSON — `https://cityjson.open3d.city/cityjson/delft.city.json`

`three_d` does not read CityJSON itself; the [`cityjson`](https://github.com/cityjson/duckdb-cityjson)
extension does, and hands over `(geometry_lod<X> BLOB, geometry_properties_lod<X> STRUCT)`
pairs. Build it from the sibling checkout and load it by path — the community-published
build is older and uses a flat `geometry` column
([why](./CITYJSON_INTEROP.md#which-cityjson-build)):

```sh
(cd ../duckdb-cityjson && GEN=ninja make release)
```
```sql
LOAD '../duckdb-cityjson/build/release/extension/cityjson/cityjson.duckdb_extension';
LOAD three_d;
```

Set up the two tables the examples below reuse. In 3DBAG the **geometry lives on
`BuildingPart` features** while the **attributes live on the parent `Building`**, so the two
are joined via `parents` / `children`:

```sql
CREATE TABLE feats AS
SELECT id, parents, children,
       geometry_lod2_2            AS geometry,
       geometry_properties_lod2_2 AS geometry_properties,
       b3_volume_lod22, b3_opp_grond
FROM read_cityjsonseq(
    'https://cityjson.open3d.city/cityjsonseq/delft.city.jsonl', lod => '2.2');

CREATE TABLE parts AS
SELECT p.id,
       p.geometry,
       ST_3DTryFromWKB(p.geometry, p.geometry_properties) AS solid,  -- SOLID_3D
       ST_Geom3DFromWKB(p.geometry)                       AS g,      -- GEOM_3D
       b.b3_volume_lod22, b.b3_opp_grond, len(b.children) AS n_parts
FROM feats p JOIN feats b ON b.id = p.parents[1]
WHERE p.geometry IS NOT NULL;
```

```sql
SELECT count(*) AS parts,
       count(*) FILTER (WHERE ST_3DValidationReport(solid).is_valid) AS valid
FROM parts;
```
```
┌───────┬───────┐
│ parts │ valid │
├───────┼───────┤
│ 1116  │ 1098  │
└───────┴───────┘
```

Most single-value examples below use one representative building:

```sql
CREATE TABLE ex AS SELECT * FROM parts WHERE id = 'NL.IMBAG.Pand.0503100000018426-0';
```

> **Always pass `lod => '...'`.** Without it, `cityjson` emits `geom_lod*` STRUCT columns
> that `ST_3DFromWKB` cannot consume. The emitted names carry the *normalised* LoD, so
> `lod => '2.2'` gives `geometry_lod2_2` and `lod => '2'` gives `geometry_lod2_0`; the
> `feats` query above aliases them once so the rest of this document can say `geometry`.

---

## The two types

| Type | Holds | Built by |
| --- | --- | --- |
| `SOLID_3D` | Closed polyhedral **solids** — shells, faces, and the topology needed for volume and validity. | `ST_3DFromWKB`, `ST_3DTryFromWKB`, `ST_3DExtrude`, `ST_MakeSolid` |
| `GEOM_3D` | **General 3D geometry** — `Point Z`, `LineString Z`, `Polygon Z`, `MultiPoint Z`, `MultiLineString Z`, `MultiPolygon Z`, and `PolyhedralSurface Z`. | `ST_Geom3DFromWKB` |

Both are named aliases over `BLOB`. Which type a function takes is not cosmetic — it decides
which functions bind:

- **Volume, validity, shell/face counts** need solid topology → `SOLID_3D`.
- **Distance, serialization, and most accessors** work on any geometry class → `GEOM_3D`.

Convert between them via WKB: `ST_Geom3DFromWKB(ST_3DAsWKB(solid))` goes solid → geom, and
`ST_MakeSolid(geom)` goes back (raising if the surface is not solid-eligible).

---

## Conventions

**Null propagation.** Any `NULL` argument yields `NULL`, with two deliberate exceptions, both
optional inputs: `ST_3DFromWKB(wkb, NULL)` builds the solid *without* metadata and returns a
non-`NULL` result — a missing sidecar is not an error — and
`ST_3DPlaceImplicit(geom, point, NULL)` places with the identity matrix.

**`TRY` variants** (`ST_3DTryFromWKB`) catch **row-level** errors and return `NULL` instead. Bind-time errors — a malformed metadata
STRUCT, a wrong argument type — still raise. There is **no** `ST_Geom3DTryFromWKB`; every
row-level error any function raises is an `Invalid Input Error`, so DuckDB's `TRY(...)` turns
it into `NULL` — `TRY(ST_Geom3DFromWKB(wkb))`, `TRY(ST_3DVolume(solid))`.

**Named arguments.** Every parameter carries the name `duckdb_functions()` reports, so arguments can be passed by
name, in any order after the positional ones:

```sql
SELECT ST_3DAsText(ST_3DTranslate(ST_Geom3DFromWKB('POINT Z (1 2 3)'::GEOMETRY), dz := 1.0, dx := 10.0, dy := 0.0));
```
```
POINT Z (11 2 4)
```

**Errors, not repair.** The extension never silently fixes geometry. `ST_3DVolume` on a
non-manifold solid raises rather than returning a plausible-looking wrong number:

```sql
SELECT ST_3DVolume(solid) FROM parts
WHERE NOT ST_3DValidationReport(solid).is_valid LIMIT 1;
```
```
Invalid Input Error: ST_3DVolume: solid is not manifold
```

Use `ST_3DTryFromWKB` and guard on `ST_3DValidationReport(...).is_valid` when exploring real
data — every real reconstruction contains some broken solids.

**Stored values come back as `BLOB`.** A `SOLID_3D` or `GEOM_3D` column written to Parquet
reads back as a plain `BLOB`, and no function accepts a `BLOB`: its cached triangulation,
bounds and validation verdict are not trusted until rebuilt. Cast it back explicitly —
`blob::SOLID_3D`, `blob::GEOM_3D` — and the cast re-derives every cache from the geometry,
raising on bytes that are not a payload of that type (`TRY_CAST` gives `NULL`). The rebuild
costs a triangulation and, for a solid, a validation, so materialise the cast once
(`CREATE TABLE … AS SELECT b::SOLID_3D AS solid …`) rather than casting in every query.
`::BLOB` on a `SOLID_3D` or `GEOM_3D` returns the payload bytes; for interchange use
`ST_3DAsWKB` / `ST_3DAsBinary`.

```sql
COPY (SELECT id, solid FROM ex) TO 'ex.parquet';
SELECT ST_3DValidationReport(solid::SOLID_3D).is_valid AS valid FROM 'ex.parquet';
```
```
┌───────┐
│ valid │
├───────┤
│ true  │
└───────┘
```

**Coexistence with `spatial`.** 3D operations use `ST_3D*` names, so `three_d` and DuckDB's
`spatial` extension load together in any order. A handful of names `spatial` does not define
(`ST_Force3D`, `ST_MakeSolid`, `ST_NDims`, `ST_CoordDim`, `ST_IsPlanar`, `ST_Geom3DFromWKB`)
stay un-prefixed.

**Discoverable from SQL.** Every public function carries a one-sentence description, real
parameter names, a runnable example and a category (the headings of the
[function index](#function-index)) in `duckdb_functions()`, so a client holding only a
database connection can find and call it:

```sql
SELECT function_name, parameters, description, examples
FROM duckdb_functions() WHERE function_name = 'st_3dvolume' LIMIT 1;
```

---

## Import / construction

The constructors take either a `BLOB` of WKB or DuckDB's native `GEOMETRY`, with or
without a `geometry_properties` sidecar, so a GeoParquet column that arrives carrying the Parquet `GEOMETRY`
logical type needs no `ST_AsWKB` in between — see
[WKB or GEOMETRY](#wkb-or-geometry) below.

| Function | Signature | Returns |
| --- | --- | --- |
| `ST_3DFromWKB` | `(wkb BLOB \| GEOMETRY)` | `SOLID_3D` |
| `ST_3DFromWKB` | `(wkb BLOB \| GEOMETRY, geometry_properties VARCHAR \| JSON \| STRUCT)` | `SOLID_3D` |
| `ST_3DTryFromWKB` | same two overloads | `SOLID_3D` or `NULL` |
| `ST_Geom3DFromWKB` | `(wkb BLOB \| GEOMETRY)` | `GEOM_3D` |
| `ST_3DGeomFromText` | `(wkt VARCHAR)` | `GEOM_3D` |

### WKB or GEOMETRY

A CityParquet package annotates its GeoParquet-legal geometry columns with the Parquet
`GEOMETRY` logical type, and DuckDB promotes such a column to its native `GEOMETRY` on
read. `geometry_lod0_0::BLOB` returns the same WKB as `ST_AsWKB(geometry_lod0_0)`, and the
constructors also accept `GEOMETRY` directly, without either:

```sql
SELECT count(*) AS n,
       ROUND(max(abs(ST_3DFootprintArea(ST_Geom3DFromWKB(geometry_lod0_0))
                     - ST_3DFootprintArea(ST_Geom3DFromWKB(ST_AsWKB(geometry_lod0_0))))), 12) AS max_abs_diff
FROM read_parquet('building.parquet') WHERE geometry_lod0_0 IS NOT NULL;
```
```
┌───────┬──────────────┐
│   n   │ max_abs_diff │
├───────┼──────────────┤
│  1115 │          0.0 │
└───────┴──────────────┘
```

Two consequences worth knowing:

- **Solid columns are unaffected**, because they never carry the annotation and could not
  survive it: DuckDB's geometry model has no polyhedral surface, and `ST_GeomFromWKB`
  raises `Unsupported geometry type in WKB` on solid bytes. A solid therefore cannot reach
  these functions as `GEOMETRY` at all; on `ST_3DFromWKB` / `ST_3DTryFromWKB` the
  `GEOMETRY` form matters for foreign GeoParquet columns, where `MultiPolygon Z` is common
  and the `TRY` form's `NULL` is the useful answer.
- Each arity is bound through a single `ANY` candidate rather than one overload per
  type, so an untyped `NULL` in either slot still binds. Anything that is neither
  `GEOMETRY` nor implicitly castable to `BLOB` is rejected at bind time, as is a sidecar
  that is neither text nor a `STRUCT` with a `shells` field.

### `ST_3DFromWKB` / `ST_3DTryFromWKB`

Builds a `SOLID_3D` from WKB. Accepts `PolyhedralSurface Z`, and `GeometryCollection Z` whose
children are all `PolyhedralSurface Z`, as ISO WKB or PostGIS EWKB. An EWKB SRID becomes the
solid's [CRS](#crs).

The optional second argument recovers structure that WKB alone cannot carry — chiefly **shell
grouping**, which is what makes interior shells (cavities) subtract from volume and
`ST_3DNumShells` report the truth. It accepts either JSON text (as `cityjson` emits) **or** a
CityParquet `geometry_properties_lod*` `STRUCT` read straight from Parquet, with no
`to_json(...)` round-trip:

```sql
-- JSON text sidecar
SELECT ST_3DVolume(ST_3DFromWKB(geometry, geometry_properties)) FROM feats WHERE geometry IS NOT NULL;

-- native STRUCT (CityParquet file)
SELECT ST_3DVolume(ST_3DFromWKB(geometry_lod2_2, geometry_properties_lod2_2))
FROM read_parquet('building.parquet') WHERE geometry_lod2_2 IS NOT NULL;
```

Only `type` and `shells` are read; `surfaces` may be `JSON` (the Parquet JSON logical type
read with the `json` extension loaded) or `VARCHAR`, and `face_semantics` and any further
fields are ignored. `shells` is read as the CityParquet `LIST<LIST<INT>>`: one inner list per solid, each the
per-shell face counts with the exterior shell first — `[[12]]`, `[[12, 4]]`, `[[12], [8, 4]]`.
A flat `[12]` is not that shape and raises (`NULL` under `TRY`), as does a face-count total
that disagrees with the WKB, or a present but empty `[]`, which describes none of the WKB's
members. Only a `NULL` or missing `shells` means "no shell metadata".

Without the sidecar a `PolyhedralSurface Z` imports as one solid with **one** shell.

`ST_3DFromWKB` raises on unparseable WKB or unsupported topology; `ST_3DTryFromWKB` returns
`NULL` for that row. Prefer the `TRY` form when scanning real data.

### `ST_Geom3DFromWKB`

Builds a `GEOM_3D` from ISO WKB, PostGIS EWKB or `GEOMETRY` (a TIN or Triangle is read as a
PolyhedralSurface or Polygon) — the entry point for the
distance, serialization, and general-accessor families. An EWKB SRID, or the CRS of a
`GEOMETRY` argument's type, becomes the value's [CRS](#crs). It is named `ST_Geom3DFromWKB` rather than `ST_GeomFromWKB` to
avoid clashing with DuckDB's built-in. **There is no `TRY` variant**; wrap it in `TRY(...)`.

Polygons are triangulated here, holes included, and the triangles are kept with the value
for the distance family. A polygon that admits no triangulation — a ring that crosses
itself, a hole outside its exterior — is refused with `polygon N cannot be triangulated`,
rather than measured wrongly later.

```sql
SELECT ST_3DGeometryType(ST_Geom3DFromWKB(geometry)) AS gtype FROM ex;
```
```
┌──────────────────────┐
│        gtype         │
├──────────────────────┤
│ ST_PolyhedralSurface │
└──────────────────────┘
```

### `ST_3DGeomFromText`

Builds a `GEOM_3D` from WKT or PostGIS EWKT, mainly for the classes DuckDB's own
`ST_GeomFromText` cannot hold: `POLYHEDRALSURFACE Z`, `TIN Z` and `TRIANGLE Z`. The other
classes `GEOM_3D` carries are read too: `POINT`, `LINESTRING`, `POLYGON` and their `MULTI`
forms. Coordinates need a Z: the `Z` tag, or three untagged ordinates. A 2D coordinate,
an `M` ordinate, or a class `GEOM_3D` does not hold is refused, with the offset in the text
where reading stopped. An `SRID=n;` prefix becomes the value's [CRS](#crs) (`EPSG:n`).

The text is turned into the WKB PostGIS would write for it and then read as
[`ST_Geom3DFromWKB`](#st_geom3dfromwkb) reads WKB, so the two agree on every value.

```sql
SELECT ST_3DGeometryType(t) AS type, ST_3DNumGeometries(t) AS patches, ST_3DArea(t) AS area, ST_3DCRS(t) AS crs
FROM (SELECT ST_3DGeomFromText('SRID=28992;TIN Z (((0 0 0, 2 0 0, 0 2 1, 0 0 0)), ((2 0 0, 2 2 1, 0 2 1, 2 0 0)))') AS t);
```
```
┌──────────────────────┬─────────┬──────────────────┬────────────┐
│         type         │ patches │       area       │    crs     │
│       varchar        │  int32  │      double      │  varchar   │
├──────────────────────┼─────────┼──────────────────┼────────────┤
│ ST_PolyhedralSurface │       2 │ 4.47213595499958 │ EPSG:28992 │
└──────────────────────┴─────────┴──────────────────┴────────────┘
```

A TIN is read as a PolyhedralSurface of triangles, and a Triangle as a Polygon: `GEOM_3D`
has no TIN or Triangle class of its own, and every measure treats them as those classes.

## Export / serialization

| Function | Signature | Returns | Notes |
| --- | --- | --- | --- |
| `ST_3DAsWKB` | `(SOLID_3D)` | `BLOB` | Canonicalized OGC WKB; **not** byte-identical to the input. |
| `ST_3DAsText` | `(GEOM_3D)` | `VARCHAR` | ISO WKT with Z. |
| `ST_3DAsGeoJSON` | `(GEOM_3D)` | `VARCHAR` | A `PolyhedralSurface` is emitted as a `MultiPolygon`. |
| `ST_3DAsBinary` | `(GEOM_3D)` | `BLOB` | OGC/ISO WKB, little-endian. |

The two **binary** exports are exact — `ST_3DAsWKB` and `ST_3DAsBinary` agree byte-for-byte
with each other and with PostGIS's `ST_AsBinary` on the same geometry. The two **text**
exports are not: ordinates are formatted with `%.9g`, so at RD coordinate magnitudes
(`62609.76675`) WKT and GeoJSON round to about 0.1 mm. Use the binary forms when the output
has to round-trip losslessly.

`ST_3DAsWKB` exports a single solid as `PolyhedralSurface Z` and a multi-solid as
`GeometryCollection Z`. It is the bridge back to `GEOM_3D` and to other tools:

```sql
SELECT ST_3DAsText(ST_3DCentroid(g)) AS centroid FROM ex;
```
```
┌────────────────────────────────────────────┐
│                  centroid                  │
├────────────────────────────────────────────┤
│ POINT Z (84995.2631 446838.844 4.73083413) │
└────────────────────────────────────────────┘
```

---

## Introspection / accessors

| Function | Signature | Returns | Accepts |
| --- | --- | --- | --- |
| `ST_3DBounds` | `(SOLID_3D \| GEOM_3D)` | `STRUCT` (6 fields) | both |
| `ST_3DExtent` | aggregate `(SOLID_3D \| GEOM_3D)` | `STRUCT` (6 fields) | both |
| `ST_3DEnvelope` | `(SOLID_3D \| GEOM_3D)` | `GEOMETRY` | both |
| `ST_3DNumSolids` | `(SOLID_3D)` | `BIGINT` | `SOLID_3D` |
| `ST_3DNumShells` | `(SOLID_3D)` | `BIGINT` | `SOLID_3D` |
| `ST_3DNumFaces` | `(SOLID_3D)` | `BIGINT` | `SOLID_3D` |
| `ST_3DXMin` / `ST_3DXMax` / `ST_3DYMin` / `ST_3DYMax` / `ST_3DZMin` / `ST_3DZMax` | `(SOLID_3D \| GEOM_3D)` | `DOUBLE` | both |
| `ST_NDims` | `(SOLID_3D \| GEOM_3D)` | `INTEGER` | both — always `3` |
| `ST_3DHasZ` | `(SOLID_3D \| GEOM_3D)` | `BOOLEAN` | both — always `true` |
| `ST_CoordDim` | `(GEOM_3D)` | `INTEGER` | `GEOM_3D` — always `3` |
| `ST_3DGeometryType` | `(GEOM_3D)` | `VARCHAR` | `GEOM_3D` |
| `ST_3DDimension` | `(GEOM_3D)` | `INTEGER` | `GEOM_3D` |
| `ST_3DNumGeometries` | `(GEOM_3D)` | `INTEGER` | `GEOM_3D` |
| `ST_3DX` / `ST_3DY` / `ST_3DZ` | `(GEOM_3D)` | `DOUBLE` | `GEOM_3D` — **Points only** |
| `ST_IsPlanar` | `(GEOM_3D)` | `BOOLEAN` | `GEOM_3D` |

Counts and bounds are **O(1) header reads** — they do not materialise the geometry, so they
are cheap to use as filters.

```sql
SELECT ST_3DNumSolids(solid) AS nsolids, ST_3DNumShells(solid) AS nshells,
       ST_3DNumFaces(solid)  AS nfaces,  ST_NDims(solid) AS ndims, ST_3DHasZ(solid) AS hasz
FROM ex;
```
```
┌─────────┬─────────┬────────┬───────┬──────┐
│ nsolids │ nshells │ nfaces │ ndims │ hasz │
├─────────┼─────────┼────────┼───────┼──────┤
│ 1       │ 1       │ 10     │ 3     │ true │
└─────────┴─────────┴────────┴───────┴──────┘
```

```sql
SELECT ST_3DGeometryType(g) AS gtype, ST_3DDimension(g) AS dim,
       ST_3DNumGeometries(g) AS ngeom, ST_CoordDim(g) AS cdim
FROM ex;
```
```
┌──────────────────────┬─────┬───────┬──────┐
│        gtype         │ dim │ ngeom │ cdim │
├──────────────────────┼─────┼───────┼──────┤
│ ST_PolyhedralSurface │ 2   │ 10    │ 3    │
└──────────────────────┴─────┴───────┴──────┘
```

`ST_3DGeometryType` returns one of `ST_Point`, `ST_LineString`, `ST_Polygon`, `ST_MultiPoint`,
`ST_MultiLineString`, `ST_MultiPolygon`, `ST_PolyhedralSurface`. `ST_3DDimension` is the
*topological* dimension: `0` for points, `1` for lines, `2` for surfaces and solids.
`ST_3DNumGeometries` counts a Multi geometry's members and a PolyhedralSurface's patches, as
PostGIS's `ST_NumGeometries` does.

> `ST_Geom3DFromWKB` does **not** accept `GeometryCollection Z` — it raises
> `ParseGeomWKB: unsupported geometry class`. Collections of polyhedral surfaces are a
> `SOLID_3D` concern; import them with `ST_3DFromWKB`, which reads them as multi-solids.

### `ST_3DBounds`

```sql
SELECT ST_3DBounds(solid) AS bounds FROM ex;
```
```
{'min_x': 84988.326625, 'min_y': 446832.685, 'min_z': -0.31699737548828466,
 'max_x': 85002.061625, 'max_y': 446844.662, 'max_z': 9.831002624511719}
```

Returns `STRUCT(min_x, min_y, min_z, max_x, max_y, max_z DOUBLE)` from the cached bounding
box, for either type; `ST_3DXMin` … `ST_3DZMax` read one field of it. Building height is the
Z extent:

```sql
SELECT ROUND(ST_3DZMax(solid) - ST_3DZMin(solid), 2) AS height_m FROM ex;   -- 10.15
```

`ST_3DExtent` is the aggregate: the box of every box in a group, `NULL` values skipped and
`NULL` for a group with none. Values in different CRSs are refused, as the functions of two
values refuse them (see [CRS](#crs)).

```sql
SELECT ST_3DExtent(solid) AS extent FROM parts;
```
```
{'min_x': 84501.553625, 'min_y': 445805.024, 'min_z': -3.7469973754882844,
 'max_x': 85675.092625, 'max_y': 446983.477, 'max_z': 95.04200262451172}
```

`ST_3DEnvelope` is the box's footprint as a core `GEOMETRY`: the rectangle of its X and Y
ranges, a line or a point where a range has no width (as PostGIS's `ST_Envelope`), `NULL`
for an empty value. It carries no CRS. It exists to hand `spatial`'s spatial join its
candidates (see [Spatial joins](#spatial-joins)).

```sql
SELECT ST_3DEnvelope(solid) AS envelope FROM ex;
```
```
POLYGON ((84988.326625 446832.685, 85002.061625 446832.685, 85002.061625 446844.662,
          84988.326625 446844.662, 84988.326625 446832.685))
```

> `ST_NDims` and `ST_3DHasZ` are constants that never read the payload — they are API-shape
> compatibility with PostGIS, not measurements. `ST_3DX`/`Y`/`Z` raise on anything that is
> not a Point.

---

## Faces, shells and parts

| Function | Signature | Returns |
| --- | --- | --- |
| `ST_3DFaces` | `(SOLID_3D)` | `STRUCT(face INTEGER, solid INTEGER, shell INTEGER, polygon GEOM_3D)[]` |
| `ST_3DFaceN` | `(SOLID_3D, n BIGINT)` | `GEOM_3D` (Polygon) |
| `ST_3DFaceAreas` | `(SOLID_3D \| GEOM_3D)` | `DOUBLE[]` |
| `ST_3DFaceNormals` | `(SOLID_3D \| GEOM_3D)` | `STRUCT(x, y, z DOUBLE)[]` |
| `ST_3DFaceSlopes` | `(SOLID_3D \| GEOM_3D)` | `DOUBLE[]` (degrees) |
| `ST_3DFaceAzimuths` | `(SOLID_3D \| GEOM_3D)` | `DOUBLE[]` (degrees) |
| `ST_3DExteriorShell` | `(SOLID_3D)` | `GEOM_3D` (PolyhedralSurface) |
| `ST_3DNumInteriorShells` | `(SOLID_3D)` | `INTEGER` |
| `ST_3DInteriorShellN` | `(SOLID_3D, n BIGINT)` | `GEOM_3D` (PolyhedralSurface) |
| `ST_3DGeometryN` | `(SOLID_3D \| GEOM_3D, n BIGINT)` | same type as input |

A solid's faces are numbered from 1 in storage order, the order `ST_3DAsWKB` writes them. A
`GEOM_3D`'s faces are its polygons: the patches of a PolyhedralSurface, the members of a
MultiPolygon, a Polygon itself; points and lines have none, and their lists are empty. Every
index is 1-based, and an index out of range gives `NULL`, as PostGIS's `ST_GeometryN` does.
Every geometry these return carries its input's CRS.

- **`ST_3DFaces`** lists each face with its solid and its shell within that solid (1 is the
  exterior, 2 onward the cavities), and the face as a `Polygon`. `UNNEST` it for one row per
  face. A face whose ring crosses itself has a `NULL` polygon, since a `GEOM_3D` cannot hold
  it; `ST_3DFaceN` gives `NULL` for it too, and so do `ST_3DExteriorShell` and
  `ST_3DInteriorShellN` for a shell holding it. A face of zero area is returned as it is.
- **`ST_3DFaceAreas`** gives each face's area: the sum of its triangles, so a hole is left out
  whichever way it is wound, and a warped face measures the surface the distance and volume
  functions see. A solid's face and the same face as a polygon have the same area. They sum
  to `ST_3DSurfaceArea` for a solid and to `ST_3DArea` for a `GEOM_3D`.
- **`ST_3DFaceNormals`** gives each face's unit normal, along its exterior ring's Newell
  vector, so it follows the right-hand rule around that ring: out of a valid solid, and into a
  cavity, which is out of the material. Holes do not lean it.
- A face with no triangles, one of zero area or one the triangulator cannot tile
  (`ST_3DValidationReport` counts either as degenerate on a solid), has a `NULL` area,
  normal, slope and azimuth.
- **`ST_3DFaceSlopes`** gives each face's tilt from horizontal, `atan2(√(nx² + ny²), |nz|)` in
  degrees: 0 for a horizontal face, 90 for a vertical one, whichever way it faces.
- **`ST_3DFaceAzimuths`** gives the compass direction a face's normal leans toward,
  `atan2(nx, ny)` in degrees clockwise from +Y (grid north in a projected CRS) in `[0, 360)`.
  It depends on the winding: a roof face of a valid solid faces the way its outward normal
  leans. A horizontal face leans nowhere and gives `NULL`; its normal has exactly zero X and Y
  when all its vertices share one Z, at any coordinate magnitude, because the Newell vector is
  taken relative to the ring's first vertex.

```sql
SELECT unnest(range(1, len(a) + 1)) AS face, ROUND(unnest(a), 2) AS area,
       ROUND(unnest(s), 1) AS slope, ROUND(unnest(z), 1) AS azimuth
FROM (SELECT ST_3DFaceAreas(solid) AS a, ST_3DFaceSlopes(solid) AS s, ST_3DFaceAzimuths(solid) AS z FROM ex);
```
```
┌───────┬────────┬────────┬─────────┐
│ face  │  area  │ slope  │ azimuth │
├───────┼────────┼────────┼─────────┤
│     1 │  87.29 │    0.0 │    NULL │
│     2 │  51.14 │   90.0 │   299.4 │
│     3 │  47.19 │   90.0 │   119.6 │
│     4 │  33.38 │   90.0 │   298.4 │
│     5 │   96.0 │   90.0 │    29.2 │
│     6 │ 109.69 │   90.0 │   209.7 │
│     7 │   1.02 │   90.0 │   197.4 │
│     8 │  38.85 │   90.0 │   119.9 │
│     9 │  15.68 │   90.0 │    29.4 │
│    10 │  87.29 │    0.5 │   310.3 │
└───────┴────────┴────────┴─────────┘
```

The floor is exactly horizontal; the roof leans 0.5° toward the north-west. Over the whole
tile, the roof area by the direction it faces, upward faces between 10° and 70° only:

```sql
SELECT CASE WHEN az < 45 OR az >= 315 THEN 'N' WHEN az < 135 THEN 'E' WHEN az < 225 THEN 'S' ELSE 'W' END AS facing,
       ROUND(sum(area)) AS roof_m2
FROM (SELECT unnest(ST_3DFaceAreas(solid)) AS area, unnest(ST_3DFaceSlopes(solid)) AS slope,
             unnest(ST_3DFaceAzimuths(solid)) AS az,
             unnest(list_transform(ST_3DFaceNormals(solid), lambda n: n.z)) AS nz
      FROM parts)
WHERE nz > 0 AND slope BETWEEN 10 AND 70
GROUP BY facing ORDER BY facing;
```
```
┌─────────┬─────────┐
│ facing  │ roof_m2 │
├─────────┼─────────┤
│ E       │ 21785.0 │
│ N       │ 24319.0 │
│ S       │ 33749.0 │
│ W       │ 25817.0 │
└─────────┴─────────┘
```

```sql
SELECT f.face, f.shell, ST_3DGeometryType(f.polygon) AS type, ROUND(ST_3DArea(f.polygon), 2) AS area
FROM (SELECT unnest(ST_3DFaces(solid)) AS f FROM ex) LIMIT 3;
```
```
┌───────┬───────┬────────────┬────────┐
│ face  │ shell │    type    │  area  │
├───────┼───────┼────────────┼────────┤
│     1 │     1 │ ST_Polygon │  87.29 │
│     2 │     1 │ ST_Polygon │  51.14 │
│     3 │     1 │ ST_Polygon │  47.19 │
└───────┴───────┴────────────┴────────┘
```

**Shells and parts.** `ST_3DExteriorShell` and `ST_3DInteriorShellN` return a shell as a
`PolyhedralSurface`, one patch per face, wound as in the solid, so `ST_MakeSolid` of the
exterior shell is the solid without its cavities. `ST_3DNumInteriorShells` counts the
cavities, from the header. All three need a `SOLID_3D` holding one solid and give `NULL` for a
collection of solids, as PostGIS's `ST_ExteriorRing` and `ST_NumInteriorRings` do for a
MultiPolygon; `ST_3DGeometryN` picks one solid first. `ST_3DGeometryN` of a `SOLID_3D` is its
n-th solid, triangulated and validated on its own; of a `GEOM_3D` it is the n-th member of a
Multi geometry or patch of a PolyhedralSurface, and the value itself for `n = 1` of a single
geometry.

```sql
SELECT ST_3DGeometryType(ST_3DExteriorShell(solid)) AS shell, ST_3DNumInteriorShells(solid) AS cavities,
       ST_3DAsText(ST_3DGeometryN(g, 1)) = ST_3DAsText(ST_3DFaceN(solid, 1)) AS same_face
FROM ex;
```
```
┌──────────────────────┬──────────┬───────────┐
│        shell         │ cavities │ same_face │
├──────────────────────┼──────────┼───────────┤
│ ST_PolyhedralSurface │        0 │ true      │
└──────────────────────┴──────────┴───────────┘
```

---

## Validation

| Function | Signature | Returns |
| --- | --- | --- |
| `ST_3DIsClosed` | `(SOLID_3D)` | `BOOLEAN` |
| `ST_3DIsManifold` | `(SOLID_3D)` | `BOOLEAN` |
| `ST_3DIsOriented` | `(SOLID_3D)` | `BOOLEAN` |
| `ST_3DIsValid` | `(SOLID_3D)` | `BOOLEAN` |
| `ST_3DValidationReport` | `(SOLID_3D)` | `STRUCT` (16 fields) |

Validation runs **at import** and is cached in the payload, so these are cheap reads rather
than recomputation.

- **Closed** — every undirected edge is used exactly twice, in opposing directions.
- **Manifold** — no edge belongs to more than two faces, and no vertex is pinched (the faces
  around it form one fan).
- **Oriented** — face winding is consistent within each shell, and every interior shell is a
  cavity: wound opposite the exterior, inside it, and not inside another interior shell.

`ST_3DIsValid` is the report's `is_valid`: every check below passes. `ST_3DValidationReport`
adds the two checks that have no predicate of their own:
connectivity (a shell's faces all connected through shared edges) and self-intersection (no
two faces crossing). The definitions and tolerances are in
[DESIGN_DOC.md §8.1](./DESIGN_DOC.md#81-the-checks); val3dity agrees with `is_valid` on
3DBAG Delft apart from planarity and its 1 mm snapping.

```sql
SELECT ST_3DIsClosed(solid) AS closed, ST_3DIsManifold(solid) AS manifold,
       ST_3DIsOriented(solid) AS oriented, ST_IsPlanar(g) AS planar
FROM ex;
```
```
┌────────┬──────────┬──────────┬────────┐
│ closed │ manifold │ oriented │ planar │
├────────┼──────────┼──────────┼────────┤
│ true   │ true     │ true     │ true   │
└────────┴──────────┴──────────┴────────┘
```

### `ST_3DValidationReport`

The one function to reach for when something fails. Returns:

| Field | Type | Meaning |
| --- | --- | --- |
| `is_valid` | `BOOLEAN` | closed **and** manifold **and** oriented **and** connected, with no degenerate faces and no self-intersections |
| `is_closed`, `is_manifold`, `is_oriented` | `BOOLEAN` | the individual checks |
| `solid_count`, `shell_count`, `face_count` | `BIGINT` | structure counts |
| `open_edge_count` | `BIGINT` | edges used by only one face |
| `non_manifold_edge_count` | `BIGINT` | edges used by more than two faces |
| `degenerate_face_count` | `BIGINT` | faces that are zero-area or collapsed, or that cannot be triangulated (a ring that crosses itself, a hole outside its face) |
| `orientation_error_count` | `BIGINT` | inconsistently wound face pairs, and interior shells that are not cavities |
| `non_manifold_vertex_count` | `BIGINT` | vertices where a shell's faces meet in more than one fan |
| `disconnected_shell_count` | `BIGINT` | shells whose faces fall into separate edge-connected parts |
| `self_intersection_count` | `BIGINT` | pairs of faces that cross each other |
| `code` | `VARCHAR` | `'VALID'` or `'INVALID'` |
| `message` | `VARCHAR` | human-readable summary |

```sql
SELECT ST_3DValidationReport(solid) AS report FROM ex;
```
```
{'is_valid': true, 'is_closed': true, 'is_manifold': true, 'is_oriented': true,
 'solid_count': 1, 'shell_count': 1, 'face_count': 10, 'open_edge_count': 0,
 'non_manifold_edge_count': 0, 'degenerate_face_count': 0, 'orientation_error_count': 0,
 'non_manifold_vertex_count': 0, 'disconnected_shell_count': 0, 'self_intersection_count': 0,
 'code': VALID, 'message': Valid solid}
```

Survey the health of a whole tile, then drill into failures:

```sql
SELECT count(*) AS parts,
       count(*) FILTER (WHERE ST_3DValidationReport(solid).is_valid) AS valid,
       count(*) FILTER (WHERE NOT ST_3DIsManifold(solid))            AS non_manifold
FROM parts;
```
```
┌───────┬───────┬──────────────┐
│ parts │ valid │ non_manifold │
├───────┼───────┼──────────────┤
│ 1116  │ 1098  │ 15           │
└───────┴───────┴──────────────┘
```

The checks **overlap** rather than partition: of those 18 invalid parts, 9 are not closed, 15
are not manifold (2 of them only at a pinched vertex), 13 are not oriented, 6 have degenerate
faces and 3 have faces that cross. A single solid commonly
fails several at once. That is why the report carries per-check counters rather than one flag
— query the individual fields to find out what is actually wrong.

---

## Measurement

| Function | Signature | Returns | Preconditions |
| --- | --- | --- | --- |
| `ST_3DVolume` | `(SOLID_3D)` | `DOUBLE` | closed + manifold + oriented, no degenerate faces, no self-intersections — **else raises** |
| `ST_3DSurfaceArea` | `(SOLID_3D)` | `DOUBLE` | no degenerate faces — **else raises** |
| `ST_3DArea` | `(SOLID_3D \| GEOM_3D)` | `DOUBLE` | on a solid, alias of `ST_3DSurfaceArea` |
| `ST_3DFootprintArea` | `(SOLID_3D \| GEOM_3D)` | `DOUBLE` | none |
| `ST_3DPerimeter` | `(SOLID_3D \| GEOM_3D)` | `DOUBLE` | none |
| `ST_3DLength` | `(GEOM_3D)` | `DOUBLE` | none |

All measurements are **Cartesian in the input units** — none reads the value's [CRS](#crs).
Reproject with `ST_3DTransform` first if the source units are not metric.

```sql
SELECT ROUND(ST_3DVolume(solid), 2)        AS volume_m3,
       ROUND(ST_3DSurfaceArea(solid), 2)   AS surface_m2,
       ROUND(ST_3DFootprintArea(solid), 2) AS footprint_m2,
       ROUND(b3_opp_grond, 2)              AS bag_ground_m2   -- 3DBAG's own figure
FROM ex;
```
```
┌───────────┬────────────┬──────────────┬───────────────┐
│ volume_m3 │ surface_m2 │ footprint_m2 │ bag_ground_m2 │
├───────────┼────────────┼──────────────┼───────────────┤
│ 881.31    │ 567.55     │ 87.29        │ 87.29         │
└───────────┴────────────┴──────────────┴───────────────┘
```

The footprint matches 3DBAG's independently computed ground area exactly. Across the whole
tile, volumes agree with 3DBAG's published `b3_volume_lod22` to a **median error of 0.017 %**:

```sql
SELECT ROUND(median(abs(ST_3DVolume(solid) - b3_volume_lod22) / b3_volume_lod22) * 100, 3)
         AS median_err_pct,
       count(*) AS n
FROM parts
WHERE n_parts = 1 AND ST_3DValidationReport(solid).is_valid AND b3_volume_lod22 > 0;
```
```
┌────────────────┬──────┐
│ median_err_pct │  n   │
├────────────────┼──────┤
│ 0.017          │ 1096 │
└────────────────┴──────┘
```

**Semantics worth knowing:**

- **`ST_3DVolume`** sums signed tetrahedral contributions. Because interior shells are wound
  opposite the exterior, cavities **subtract automatically** — no "this shell is a hole" flag
  is needed. Multi-solid values sum their members. The tetrahedra are referenced to **each
  shell's own first vertex** rather than to the coordinate origin, so the result does not
  depend on where the model sits, nor on how far apart a multi-solid's parts are: a building
  measures the same in RD New easting/northing as it does at the origin. Summing about the
  absolute origin would cancel away roughly nine of the sixteen available digits at
  projected-CRS magnitudes. Under rigid motions the volume is preserved to **~1e-11
  relative** — measured max 3.0e-11 over all 1098 valid solids of the Delft tile under
  rotation, and *exactly* 0 under translation. Rotation is the looser of the two because
  rotating absolute RD coordinates injects `|p|·eps` rounding into the vertices themselves,
  before any measurement runs; that floor is a property of the coordinates, not of the volume
  sum.
- **`ST_3DSurfaceArea` / `ST_3DArea`** sum *all* faces and are **not** shell-aware: cavity
  walls count toward surface area. Only volume distinguishes shell roles.
- **`ST_3DFootprintArea`** is the 2D XY-projected ground area. It has no validity
  precondition, so it still works on solids that fail validation.
- **`ST_3DArea(GEOM_3D)`** sums the polygons of a `Polygon`, `MultiPolygon` or
  `PolyhedralSurface`, holes excluded, from the triangles cached at import; `0` for points and
  lines. On the building above it equals the solid's surface area.
- **`ST_3DPerimeter`** measures the *boundary*, as SQL/MM defines a perimeter: on a solid and
  on a `PolyhedralSurface`, the edges used by exactly one face, so a **closed solid or surface
  returns `0`** and the function is meaningful for open shells; on a `Polygon` or
  `MultiPolygon`, every ring, holes included. PostGIS's `ST_3DPerimeter` sums every ring of a
  `PolyhedralSurface` instead, closed or not; on polygons the two agree.
- **`ST_3DLength`** is `GEOM_3D`-only and applies to `LineString`/`MultiLineString`. It
  returns `0.0` (not an error) for any other geometry class.

```sql
SELECT ROUND(ST_3DPerimeter(solid), 4) AS perimeter,   -- 0: the shell is closed
       ROUND(ST_3DLength(g), 4)        AS length       -- 0: not a linestring
FROM ex;
```
```
┌───────────┬────────┐
│ perimeter │ length │
├───────────┼────────┤
│ 0.0       │ 0.0    │
└───────────┴────────┘
```

```sql
SELECT ROUND(ST_3DArea(g), 2) AS area_geom, ROUND(ST_3DArea(solid), 2) AS area_solid,
       ROUND(ST_3DPerimeter(g), 4) AS perimeter_geom
FROM ex;
```
```
┌───────────┬────────────┬────────────────┐
│ area_geom │ area_solid │ perimeter_geom │
├───────────┼────────────┼────────────────┤
│    567.55 │     567.55 │            0.0 │
└───────────┴────────────┴────────────────┘
```

---

## Distance / relationships

All take **`GEOM_3D`** — build it with `ST_Geom3DFromWKB`. `ST_3DDistance`, `ST_3DDWithin`,
`ST_3DIntersects` and `ST_3DDisjoint` also take a **`SOLID_3D`** on either side, as the volume
it bounds (see [Solids are volumes](#solids-are-volumes)). The two values must be in one
[CRS](#crs).

| Function | Signature | Returns |
| --- | --- | --- |
| `ST_3DDistance` | `(GEOM_3D \| SOLID_3D, GEOM_3D \| SOLID_3D)` | `DOUBLE` |
| `ST_3DMaxDistance` | `(GEOM_3D, GEOM_3D)` | `DOUBLE` |
| `ST_3DDWithin` | `(GEOM_3D \| SOLID_3D, GEOM_3D \| SOLID_3D, dist DOUBLE)` | `BOOLEAN` |
| `ST_3DDFullyWithin` | `(GEOM_3D, GEOM_3D, dist DOUBLE)` | `BOOLEAN` |
| `ST_3DIntersects` | `(GEOM_3D \| SOLID_3D, GEOM_3D \| SOLID_3D)` | `BOOLEAN` |
| `ST_3DDisjoint` | `(GEOM_3D \| SOLID_3D, GEOM_3D \| SOLID_3D)` | `BOOLEAN` |
| `ST_3DClosestPoint` | `(GEOM_3D, GEOM_3D)` | `GEOM_3D` (Point) |
| `ST_3DShortestLine` | `(GEOM_3D, GEOM_3D)` | `GEOM_3D` (LineString) |

```sql
CREATE TABLE pair AS
SELECT (SELECT g FROM parts WHERE id = 'NL.IMBAG.Pand.0503100000018426-0') AS a,
       (SELECT g FROM parts WHERE id = 'NL.IMBAG.Pand.0503100000005156-0') AS b;

SELECT ROUND(ST_3DDistance(a, b), 2)    AS dist_m,
       ROUND(ST_3DMaxDistance(a, b), 2) AS max_dist_m,
       ST_3DDWithin(a, b, 500)          AS within_500m,
       ST_3DIntersects(a, b)            AS intersects
FROM pair;
```
```
┌────────┬────────────┬─────────────┬────────────┐
│ dist_m │ max_dist_m │ within_500m │ intersects │
├────────┼────────────┼─────────────┼────────────┤
│ 308.68 │ 332.18     │ true        │ false      │
└────────┴────────────┴─────────────┴────────────┘
```

`ST_3DClosestPoint` returns the point **on the first geometry** nearest the second;
`ST_3DShortestLine` returns the 2-vertex segment joining them:

```sql
SELECT ST_3DAsText(ST_3DClosestPoint(a, b))     AS closest,
       ROUND(ST_3DLength(ST_3DShortestLine(a, b)), 2) AS line_len
FROM pair;
```
```
┌────────────────────────────────────────────┬──────────┐
│                  closest                   │ line_len │
├────────────────────────────────────────────┼──────────┤
│ POINT Z (84988.3266 446838.07 0.691002625) │ 308.68   │
└────────────────────────────────────────────┴──────────┘
```

**Notes.** `ST_3DDWithin` prunes on bounding boxes and exits on the first hit, so it is much
cheaper than `ST_3DDistance(...) <= d` — prefer it for proximity filters. A **negative**
threshold returns `false` rather than raising, for both `DWithin` variants.
`ST_3DDFullyWithin` asks whether the *maximum* distance is within the threshold, i.e. whether
one geometry lies entirely inside a buffer of the other.

`ST_3DIntersects` is **exact**: it decides whether the two closed geometries share a point
with exact orientation predicates, not by comparing a distance with a tolerance. Touching
counts at any coordinate magnitude, and a gap of one unit in the last place is a gap.
`ST_3DDWithin(a, b, 0)` is a distance computed in doubles and can differ from it where the
two geometries are within rounding of touching; use `ST_3DIntersects` for the predicate.

A polygon is measured as the surface it encloses: a point in a concave face's notch or over
a hole is at its distance from the face's edges, not 0. `ST_3DClosestPoint(a, b)` is the point
of `a` nearest `b`. On planar faces the family agrees with PostGIS; PostGIS is not the
reference for faces with holes, whose interior rings its 3D point-in-polygon test ignores in
the face's plane ([TEST_COVERAGE.md](./TEST_COVERAGE.md#live-comparison-on-real-data--distance-solids-measures-and-elements)).

Find neighbours of a reference building:

```sql
WITH ref AS (SELECT g FROM parts WHERE id = 'NL.IMBAG.Pand.0503100000018426-0')
SELECT p.id FROM parts p, ref
WHERE p.id <> 'NL.IMBAG.Pand.0503100000018426-0'
  AND ST_3DDWithin(p.g, ref.g, 5.0);
```

### Spatial joins

`ST_3DDWithin` in a join condition is a nested loop over every pair: its box test is cheap,
but a city has billions of pairs. With `spatial` loaded, join on `ST_DWithin` of the
[envelopes](#st_3dbounds) instead, which DuckDB plans as a `SPATIAL_JOIN` over an R-tree,
and refine the candidates with `ST_3DDWithin`. The envelopes' 2D distance never exceeds the
values' 3D distance, so the candidates hold every pair the 3D test accepts. Every building
pair in the tile within 5 m of each other:

```sql
LOAD spatial;
SELECT count(*) AS pairs
FROM parts a JOIN parts b ON ST_DWithin(ST_3DEnvelope(a.g), ST_3DEnvelope(b.g), 5.0)
WHERE CASE WHEN a.id < b.id THEN ST_3DDWithin(a.g, b.g, 5.0) ELSE false END;
```
```
┌───────┐
│ pairs │
├───────┤
│ 1423  │
└───────┘
```

Keep the join condition to the envelope predicate alone. DuckDB folds any other comparison
of the two sides, such as `a.id < b.id` in the `WHERE` clause, into the join and then plans a
merge or nested-loop join; inside the `CASE` it stays a filter on the candidates. `EXPLAIN`
shows which join was chosen.

### Solids are volumes

With a `SOLID_3D` argument the three functions measure the volume its shells bound, cavities
excluded: a geometry inside it is at distance 0 and intersects it, while its surface, as a
`GEOM_3D`, is metres away. The reference building's surface centroid lies inside it:

```sql
SELECT ROUND(ST_3DDistance(solid, c), 3) AS to_volume,
       ROUND(ST_3DDistance(g, c), 3)     AS to_surface
FROM ex, (SELECT ST_3DCentroid(g) AS c FROM ex);
```
```
┌───────────┬────────────┐
│ to_volume │ to_surface │
├───────────┼────────────┤
│ 0.0       │ 4.111      │
└───────────┴────────────┘
```

Two solids intersect when they share a point, so terraced houses sharing a wall do:

```sql
SELECT count(*) FILTER (WHERE ST_3DIntersects(p.solid, ex.solid)) AS touching,
       count(*) FILTER (WHERE ST_3DDWithin(p.solid, ex.solid, 50)) AS within_50m
FROM parts p, ex
WHERE p.id <> ex.id AND ST_3DValidationReport(p.solid).is_valid;
```
```
┌──────────┬────────────┐
│ touching │ within_50m │
├──────────┼────────────┤
│ 2        │ 49         │
└──────────┴────────────┘
```

A volume is defined only for a valid solid, so an invalid one **raises** (`TRY` gives
`NULL`), even when the pair is far apart; filter with
`ST_3DValidationReport(solid).is_valid`, as above. Containment is decided exactly, so
`ST_3DIntersects` stays exact with solids. `ST_3DMaxDistance`, `ST_3DDFullyWithin`,
`ST_3DClosestPoint` and `ST_3DShortestLine` take only `GEOM_3D`.

---

## CRS

| Function | Signature | Returns |
| --- | --- | --- |
| `ST_3DCRS` | `(SOLID_3D \| GEOM_3D)` | `VARCHAR` |
| `ST_3DSRID` | `(SOLID_3D \| GEOM_3D)` | `INTEGER` |
| `ST_3DSetCRS` | `(SOLID_3D \| GEOM_3D, crs VARCHAR)` | same type as input |

Every value carries a CRS, or none. It comes in with the input: an EWKB SRID becomes
`'EPSG:<srid>'`, and a `GEOMETRY('…')` argument brings its type's CRS. ISO WKB and a plain
`GEOMETRY` bring none. `ST_3DSetCRS` labels a value without moving its coordinates, and an
empty string removes the label.

- **`ST_3DCRS`** returns the CRS string, `NULL` when there is none.
- **`ST_3DSRID`** returns `n` for a CRS `'EPSG:n'` and `0` otherwise, PostGIS's unknown SRID.
- Every function that returns a geometry keeps its input's CRS. `ST_3DTransform` labels its
  result with the target CRS, and `ST_3DPlaceImplicit` gives its result the reference point's.
- Functions of two values (`ST_3DDistance`, `ST_3DIntersects`, `ST_3DDisjoint`, `ST_3DDWithin`,
  `ST_3DMaxDistance`, `ST_3DDFullyWithin`, `ST_3DClosestPoint`, `ST_3DShortestLine`) **raise**
  when the two CRSs differ. A value without a CRS differs from one with, as in PostGIS. Two
  spellings of one CRS, such as an authority code in either case and a PROJJSON carrying that
  id, are equal.
- The exports (`ST_3DAsWKB`, `ST_3DAsBinary`, `ST_3DAsText`, `ST_3DAsGeoJSON`) write ISO forms,
  which carry no CRS, as PostGIS's `ST_AsBinary` and `ST_AsText` do. Keep `ST_3DCRS` beside
  them, or set it again with `ST_3DSetCRS` after reading them back.

The 3DBAG tile is in `EPSG:7415` (RD New + NAP height), which its WKB does not carry:

```sql
SELECT ST_3DCRS(s) AS crs, ST_3DSRID(s) AS srid,
       ST_3DAsText(ST_3DCentroid(ST_3DTransform(s, 4326))) AS wgs84_centroid,
       ST_3DCRS(ST_3DTransform(s, 4326)) AS crs_after
FROM (SELECT ST_3DSetCRS(solid, 'EPSG:7415') AS s FROM ex);
```
```
┌───────────┬───────┬───────────────────────────────────────────┬───────────┐
│    crs    │ srid  │              wgs84_centroid               │ crs_after │
├───────────┼───────┼───────────────────────────────────────────┼───────────┤
│ EPSG:7415 │  7415 │ POINT Z (4.3676484 52.0054997 4.73119833) │ EPSG:4326 │
└───────────┴───────┴───────────────────────────────────────────┴───────────┘
```

```sql
SELECT ST_3DDistance(ST_3DSetCRS(a.g, 'EPSG:7415'), b.g) FROM ex a, ex b;
```
```
Invalid Input Error: ST_3DDistance: the inputs have different CRSs (EPSG:7415 and none);
bring them to one with ST_3DTransform, or set one with ST_3DSetCRS
```

---

## Transform / construct

| Function | Signature | Returns |
| --- | --- | --- |
| `ST_3DTranslate` | `(SOLID_3D \| GEOM_3D, dx, dy, dz DOUBLE)` | same type as input |
| `ST_3DScale` | `(SOLID_3D \| GEOM_3D, sx, sy, sz DOUBLE)` | same type as input |
| `ST_3DRotateX/Y/Z` | `(SOLID_3D \| GEOM_3D, radians DOUBLE)` | same type as input |
| `ST_3DTransform` | `(SOLID_3D \| GEOM_3D, [src,] tgt)` | same type as input |
| `ST_3DPlaceImplicit` | `(relative_geometry SOLID_3D \| GEOM_3D, reference_point GEOM_3D, transformation_matrix DOUBLE[])` | same type as `relative_geometry` |
| `ST_3DExtrude` | `(GEOM_3D polygon, height DOUBLE)` | `SOLID_3D` |
| `ST_MakeSolid` | `(GEOM_3D)` | `SOLID_3D` |
| `ST_3DCentroid` | `(SOLID_3D \| GEOM_3D)` | `GEOM_3D` (Point) |
| `ST_3DConvexHull` | `(SOLID_3D \| GEOM_3D)` | `GEOM_3D` |
| `ST_3DFootprintHull` | `(GEOM_3D)` | `GEOM_3D` |
| `ST_Force3D` | `(GEOM_3D)` | `GEOM_3D` |

All transforms are **type-preserving** and operate per-vertex, keeping topology intact.

```sql
SELECT ROUND(ST_3DVolume(ST_3DTranslate(solid, 100, 50, 0)), 2) AS translated,
       ROUND(ST_3DVolume(ST_3DScale(solid, 2, 2, 2)), 2)        AS scaled_2x,
       ROUND(ST_3DVolume(ST_3DRotateZ(solid, pi()/4)), 2)       AS rotated
FROM ex;
```
```
┌────────────┬───────────┬─────────┐
│ translated │ scaled_2x │ rotated │
├────────────┼───────────┼─────────┤
│ 881.31     │ 7050.51   │ 881.31  │
└────────────┴───────────┴─────────┘
```

Translation and rotation are rigid motions and preserve volume; scaling by 2 in each axis
multiplies it by 8, as expected. Rotations follow the PostGIS convention: **right-handed,
counter-clockwise, in radians**.

### `ST_3DPlaceImplicit` — implicit geometries

An implicit geometry stores a *relative geometry* once, in its own local coordinates, and
places it at each object by a **transformation matrix** and a **reference point**. In a
CityParquet package the relative geometries are the rows of `implicit_geometries.parquet`, and
an object's `implicit_geometry` struct carries the `id` to join on, the reference `point` (WKB
`Point Z`) and the `transformationMatrix`. `ST_3DPlaceImplicit` materialises one instance:
every relative vertex `v` lands at `M · v + p`.

- `transformation_matrix` is a flat, **row-major** 4×4: exactly 16 finite values whose last row
  is `0, 0, 0, 1`. Its own translation column is applied before the reference point is added.
  A `NULL` matrix is the **identity** — the matrix is optional — which makes this one of the
  functions whose `NULL` handling is not plain propagation. A `NULL` element, a length other
  than 16, or a projective last row raises.
- `reference_point` must be a `GEOM_3D` `Point`; a `NULL` relative geometry or reference point
  gives `NULL`.
- The result has the relative geometry's type. A `SOLID_3D` is re-validated: a mirroring
  matrix keeps it valid (winding stays consistent, only its handedness flips), and its volume
  scales by `|det M|`. A **singular** matrix — `|det M|` negligible against the product of its
  row lengths, as for any projection — **raises** for a `SOLID_3D`: it would collapse the solid
  to zero volume, and a projection can do that while every face stays non-degenerate, so the
  result would pass validation. A `GEOM_3D` has no volume to lose and is placed regardless.
- The result is in the reference point's coordinates — the file CRS in a CityParquet package —
  while the relative geometry, in local coordinates, is exempt from it.

```sql
SELECT o.id, ST_3DAsText(ST_3DCentroid(ST_3DPlaceImplicit(
         ST_Geom3DFromWKB(t.geometry_lod3_0),
         ST_Geom3DFromWKB(o.implicit_geometry.point),
         o.implicit_geometry.transformationMatrix))) AS placed_centroid
FROM 'test/data/railway_implicit/vegetation.parquet' o
JOIN 'test/data/railway_implicit/implicit_geometries.parquet' t ON t.id = o.implicit_geometry.id
ORDER BY o.id LIMIT 3;
```
```
┌────────────────────────────┬─────────────────────────────────────────────┐
│             id             │               placed_centroid               │
├────────────────────────────┼─────────────────────────────────────────────┤
│ GMLID_SO0107241_3793_12555 │ POINT Z (1.15844898 6.6314892 9.0743353)    │
│ GMLID_SO0124800_3522_13577 │ POINT Z (0.828664787 7.54983942 9.32059222) │
│ GMLID_SO015374_872_14131   │ POINT Z (0.803664787 6.93783942 9.16959222) │
└────────────────────────────┴─────────────────────────────────────────────┘
```

The railway fixture's matrices are all the identity. A scaling matrix, by hand:

```sql
SELECT ST_3DAsText(ST_3DPlaceImplicit(
    ST_Geom3DFromWKB('POLYGON Z ((-1 -1 0, 1 -1 0, 1 1 0, -1 1 0, -1 -1 0))'::GEOMETRY),
    ST_Geom3DFromWKB('POINT Z (5 6 0)'::GEOMETRY),
    [2.0, 0, 0, 0, 0, 2, 0, 0, 0, 0, 2, 0, 0, 0, 0, 1])) AS placed;
```
```
┌─────────────────────────────────────────────────┐
│                     placed                      │
├─────────────────────────────────────────────────┤
│ POLYGON Z ((3 4 0, 7 4 0, 7 8 0, 3 8 0, 3 4 0)) │
└─────────────────────────────────────────────────┘
```

### `ST_3DTransform` — CRS reprojection

```
ST_3DTransform(geom, target_srid INTEGER)                      → same type
ST_3DTransform(geom, target_crs VARCHAR)                       → same type
ST_3DTransform(geom, source_srid INTEGER, target_srid INTEGER) → same type
ST_3DTransform(geom, source_crs VARCHAR, target_crs VARCHAR)   → same type
```

PROJ-backed reprojection with **PostGIS's horizontal-only semantics: X and Y are reprojected,
Z is passed through unchanged.** There is no vertical datum or geoid transformation.

The string form takes an authority code (`'EPSG:28992'`) or a WKT2 CRS string; the integer
form is shorthand for `EPSG:<n>`. PROJ **pipeline** strings (`'+proj=pipeline …'`) are *not*
accepted. Axis order is normalised to easting/northing, so `EPSG:4326` behaves as
**(lon, lat)** — the GIS convention, not the authority's declared order.

The two-argument forms read the source CRS from the value and raise when it has none. The
three-argument forms take it from the call, and raise when the value carries a different one.
The result carries the target CRS (see [CRS](#crs)).

```sql
-- Dutch RD New (EPSG:28992) → WGS84
SELECT ST_3DAsText(ST_3DCentroid(ST_3DTransform(g, 28992, 4326))) AS wgs84_centroid FROM ex;
```
```
┌────────────────────────────────────────────┐
│              wgs84_centroid                │
├────────────────────────────────────────────┤
│ POINT Z (4.36764676 52.0055004 4.74002571) │
└────────────────────────────────────────────┘
```

Note the unchanged Z (`4.74`) alongside the reprojected lon/lat. Solids are **re-validated**
after reprojection, because a handedness-flipping CRS can invert face winding. Invalid CRS
codes raise; there is no `TRY` variant.

### `ST_3DExtrude` — footprints to LoD1

Extrudes a `Polygon Z` vertically into a closed prism. Raises if the input is not a polygon,
has fewer than 3 distinct exterior-ring vertices, or `height <= 0`.

```sql
SELECT ST_3DNumFaces(ST_3DExtrude(ST_3DFootprintHull(g), 10.0))            AS prism_faces,
       ROUND(ST_3DVolume(ST_3DExtrude(ST_3DFootprintHull(g), 10.0)), 2)    AS prism_vol,
       ROUND(ST_3DFootprintArea(ST_3DFootprintHull(g)) * 10.0, 2)          AS footprint_x_height
FROM ex;
```
```
┌─────────────┬───────────┬────────────────────┐
│ prism_faces │ prism_vol │ footprint_x_height │
├─────────────┼───────────┼────────────────────┤
│ 8           │ 901.09    │ 901.09             │
└─────────────┴───────────┴────────────────────┘
```

The result is closed, manifold, and oriented — a valid `SOLID_3D`.

### `ST_MakeSolid`

Promotes a closed `PolyhedralSurface` `GEOM_3D` to `SOLID_3D`. **No silent repair**: it raises
unless the surface is already closed, manifold, and oriented. Round-tripping a solid through
WKB and back is lossless:

```sql
SELECT ST_3DVolume(ST_MakeSolid(ST_Geom3DFromWKB(ST_3DAsWKB(solid)))) = ST_3DVolume(solid)
         AS roundtrip_ok
FROM ex;
```
```
┌──────────────┐
│ roundtrip_ok │
├──────────────┤
│ true         │
└──────────────┘
```

### `ST_3DConvexHull` and `ST_3DFootprintHull`

`ST_3DConvexHull` is the 3D hull of every vertex: a closed `PolyhedralSurface Z` whose faces
are convex and wound outward, so `ST_MakeSolid` accepts it. Coplanar faces of the hull are
merged into one polygon, and every hull vertex is an input vertex. Exact predicates decide
which side of a face a point lies on, so a point a rounding error above a face is kept and
one on it is not. Input whose vertices are all coplanar gives a `Polygon Z`, all collinear a
`LineString Z`, a single point a `Point Z`. Because the hull is exact, three input points
that are collinear to within rounding but not exactly give a sliver face: on the Vienna
model one hull face is 14 m round and 4e-11 m² in area, which validation counts as
degenerate, so `ST_3DVolume(ST_MakeSolid(hull))` raises for that building.

```sql
SELECT ST_3DGeometryType(h)                    AS hull_type,
       ST_3DNumFaces(ST_MakeSolid(h))          AS hull_faces,
       ROUND(ST_3DVolume(ST_MakeSolid(h)), 2)  AS hull_vol,
       ROUND(ST_3DVolume(solid), 2)            AS solid_vol
FROM (SELECT solid, ST_3DConvexHull(solid) AS h FROM ex);
```
```
┌──────────────────────┬────────────┬──────────┬───────────┐
│      hull_type       │ hull_faces │ hull_vol │ solid_vol │
├──────────────────────┼────────────┼──────────┼───────────┤
│ ST_PolyhedralSurface │ 11         │ 909.68   │ 881.31    │
└──────────────────────┴────────────┴──────────┴───────────┘
```

`ST_3DFootprintHull` is the 2D hull of the vertices' XY projection, returned as a `Polygon Z`
at the input's **minimum Z** — the footprint `ST_3DExtrude` turns into an LoD1 block.
Degenerate inputs return a `LineString Z` or `Point Z`.

```sql
SELECT ROUND(ST_3DFootprintArea(ST_3DFootprintHull(g)), 2) AS hull_area,
       ROUND(ST_3DFootprintArea(g), 2)                     AS footprint_area
FROM ex;
```
```
┌───────────┬────────────────┐
│ hull_area │ footprint_area │
├───────────┼────────────────┤
│ 90.11     │ 87.29          │
└───────────┴────────────────┘
```

### `ST_3DCentroid`, `ST_Force3D`

- **`ST_3DCentroid`** — for a `SOLID_3D`, the centroid of its **volume**, cavities left out;
  an unclosed solid raises. For a `GEOM_3D`, area-weighted for surfaces, length-weighted for
  lines, vertex average for points. Returns a `Point Z`. A solid's volume centroid and its
  surface's area centroid differ:

```sql
SELECT ST_3DAsText(ST_3DCentroid(solid)) AS volume_centroid,
       ST_3DAsText(ST_3DCentroid(g))     AS surface_centroid
FROM ex;
```
```
┌────────────────────────────────────────────┬────────────────────────────────────────────┐
│              volume_centroid               │              surface_centroid              │
├────────────────────────────────────────────┼────────────────────────────────────────────┤
│ POINT Z (84995.3322 446838.785 4.73119834) │ POINT Z (84995.2631 446838.844 4.73083413) │
└────────────────────────────────────────────┴────────────────────────────────────────────┘
```

- **`ST_Force3D`** — currently an identity round-trip, since `GEOM_3D` already stores XYZ.
  Reserved for future 2D inputs.

## SQL/MM profile

The function names follow SQL/MM Part 3 and PostGIS's `ST_3D*` family. What this extension
covers, and where it departs from either, is listed here so a query ported from PostGIS
does not change meaning silently.

**Covered.** Import from WKB, EWKB, `GEOMETRY` and WKT/EWKT; export as WKB, WKT and
GeoJSON; the 3D accessors (type, dimension, coordinates, bounds, element counts, the
`N`-th element, faces and shells); 3D length, area, perimeter, volume, centroid; 3D
distance and its predicates; validity of solids; affine transforms and reprojection; a
stored CRS.

**Not covered.** The boolean operations (`ST_3DUnion`, `ST_3DIntersection`,
`ST_3DDifference`) and the other names under [Not implemented](#function-index).
`ST_3DFromText` and `ST_3DSetSRID` are not offered: a WKT solid has no standard spelling,
and `ST_3DSetCRS` takes an `EPSG:n` code or any other CRS string where an SRID takes only
a number.

**Departures.**

| Topic | This extension | PostGIS |
| --- | --- | --- |
| `ST_3DPerimeter` of a surface | Length of the boundary: edges used by one face only, so a closed shell gives 0 (SQL/MM) | Sum of every ring |
| TIN, Triangle | Read as PolyhedralSurface, Polygon; written back as those | Kept as their own classes |
| Slope, azimuth | `ST_3DFaceSlopes` and `ST_3DFaceAzimuths`, in degrees; azimuth clockwise from +Y, in [0, 360) | No per-face functions |
| Shell accessors on a multi-solid | `ST_3DExteriorShell`, `ST_3DNumInteriorShells`, `ST_3DInteriorShellN` give `NULL` | No shell functions (PostGIS 3.4) |
| Mixed CRSs | Binary functions and `ST_3DExtent` raise an error | Binary functions raise an error on mixed SRIDs |

---

## Function index

75 public functions.

| Category | Functions |
| --- | --- |
| **Import** | `ST_3DFromWKB`, `ST_3DTryFromWKB`, `ST_Geom3DFromWKB`, `ST_3DGeomFromText` |
| **Export** | `ST_3DAsWKB`, `ST_3DAsText`, `ST_3DAsGeoJSON`, `ST_3DAsBinary` |
| **Introspection** | `ST_3DBounds`, `ST_3DExtent` (aggregate), `ST_3DEnvelope`, `ST_3DXMin`, `ST_3DXMax`, `ST_3DYMin`, `ST_3DYMax`, `ST_3DZMin`, `ST_3DZMax`, `ST_3DNumSolids`, `ST_3DNumShells`, `ST_3DNumFaces`, `ST_NDims`, `ST_3DHasZ`, `ST_CoordDim`, `ST_3DGeometryType`, `ST_3DDimension`, `ST_3DNumGeometries`, `ST_3DX`, `ST_3DY`, `ST_3DZ`, `ST_IsPlanar` |
| **Faces** | `ST_3DFaces`, `ST_3DFaceN`, `ST_3DFaceAreas`, `ST_3DFaceNormals`, `ST_3DFaceSlopes`, `ST_3DFaceAzimuths` |
| **Shells and parts** | `ST_3DExteriorShell`, `ST_3DNumInteriorShells`, `ST_3DInteriorShellN`, `ST_3DGeometryN` |
| **Validation** | `ST_3DIsValid`, `ST_3DIsClosed`, `ST_3DIsManifold`, `ST_3DIsOriented`, `ST_3DValidationReport` |
| **Measurement** | `ST_3DVolume`, `ST_3DSurfaceArea`, `ST_3DArea`, `ST_3DFootprintArea`, `ST_3DPerimeter`, `ST_3DLength` |
| **Distance** | `ST_3DDistance`, `ST_3DMaxDistance`, `ST_3DDWithin`, `ST_3DDFullyWithin`, `ST_3DIntersects`, `ST_3DDisjoint`, `ST_3DClosestPoint`, `ST_3DShortestLine` |
| **CRS** | `ST_3DCRS`, `ST_3DSRID`, `ST_3DSetCRS` |
| **Transform / construct** | `ST_3DTranslate`, `ST_3DScale`, `ST_3DRotateX`, `ST_3DRotateY`, `ST_3DRotateZ`, `ST_3DTransform`, `ST_3DPlaceImplicit`, `ST_3DExtrude`, `ST_MakeSolid`, `ST_3DCentroid`, `ST_3DConvexHull`, `ST_3DFootprintHull`, `ST_Force3D` |

**Not implemented.** These PostGIS names appear in comparison tables but are **not**
registered: `ST_3DUnion` /
`ST_3DIntersection` / `ST_3DDifference`, `ST_3DLongestLine`, `ST_3DFromText`, `ST_Affine`,
`ST_SRID` / `ST_SetSRID` (see `ST_3DSRID` / `ST_3DSetCRS`), `ST_AsGML` / `ST_AsKML` / `ST_AsX3D`, `ST_HasM` / `ST_M`,
`ST_Boundary`, `ST_PointOnSurface`, `ST_Tesselate`, `ST_StraightSkeleton`,
`ST_ApproximateMedialAxis`. The boolean operations await a decision on a
CGAL/SFCGAL backend — see [DESIGN_DOC.md §11](./DESIGN_DOC.md#11-roadmap).

---

## Appendix: test fixtures

The `ST_AsWKB*` family (`ST_AsWKBPolyhedralTetra`, `ST_AsWKBOpenTetra`, `ST_AsWKBHollowCube`,
`ST_AsWKBMultiCube`, `ST_AsWKBTwoCubes`, `ST_AsWKBPointZ`, `ST_AsWKBLineZ`, `ST_AsWKBMultiLineZ`,
`ST_AsWKBPolygonZ`, `ST_AsWKBWarpedPolygonZ`, `ST_AsWKBMultiPointZ`, `ST_AsWKBMultiPolygonZ`)
generates small WKB fixtures for the test suite. `ST_AsWKBMultiCube` has a second overload,
`ST_AsWKBMultiCube(separation DOUBLE)`, which places the two cubes `separation` apart on every
axis; total volume stays 16 at any separation, which is what the conditioning regression in
`test/sql/st_3d_multisolid.test` pins. `ST_AsWKBTwoCubes(offset DOUBLE)` puts two 2 × 2 × 2
cubes, at the origin and at `offset` on every axis, into one `PolyhedralSurface Z`: they
overlap below 2, touch at a vertex at 2 and are apart above it, which is what
`test/sql/st_3d_validation.test` pins the self-intersection and connectivity checks with.

**These are not public API.** They are registered only when the `THREE_D_TEST_FIXTURES`
environment variable is set. The `Makefile` exports it, so every `make` target has them; a
directly-invoked `build/release/duckdb` does not, and SQL test files declaring
`require-env THREE_D_TEST_FIXTURES` are silently skipped without it.

```sh
THREE_D_TEST_FIXTURES=1 ./build/release/duckdb -unsigned \
  -c "SELECT ST_3DVolume(ST_3DFromWKB(ST_AsWKBHollowCube()));"   -- 56.0
```
