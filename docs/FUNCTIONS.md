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
- [Validation](#validation)
- [Measurement](#measurement)
- [Distance / relationships](#distance--relationships)
- [Transform / construct](#transform--construct)
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

**Null propagation.** Any `NULL` argument yields `NULL`, with one deliberate exception:
`ST_3DFromWKB(wkb, NULL)` builds the solid *without* metadata and returns a non-`NULL`
result — a missing sidecar is not an error.

**`TRY` variants** (`ST_3DTryFromWKB`) catch **row-level** errors and return `NULL` instead. Bind-time errors — a malformed metadata
STRUCT, a wrong argument type — still raise. There is **no** `ST_Geom3DTryFromWKB`.

**Errors, not repair.** The extension never silently fixes geometry. `ST_3DVolume` on a
non-manifold solid raises rather than returning a plausible-looking wrong number:

```sql
SELECT ST_3DVolume(solid) FROM parts
WHERE NOT ST_3DValidationReport(solid).is_valid LIMIT 1;
```
```
Invalid Error: ST_3DVolume: solid is not manifold
```

Use `ST_3DTryFromWKB` and guard on `ST_3DValidationReport(...).is_valid` when exploring real
data — every real reconstruction contains some broken solids.

**`BLOB` overloads.** Most `SOLID_3D` consumers also accept raw `BLOB`, so stored payloads
keep working. Note that for `ST_3DTranslate` / `ST_3DScale` / `ST_3DRotate*` the `BLOB`
overload is bound to the **solid** executor — pass `GEOM_3D` values as their own type, not as
`BLOB`.

**Coexistence with `spatial`.** 3D operations use `ST_3D*` names, so `three_d` and DuckDB's
`spatial` extension load together in any order. A handful of names `spatial` does not define
(`ST_Force3D`, `ST_MakeSolid`, `ST_NDims`, `ST_CoordDim`, `ST_IsPlanar`, `ST_Geom3DFromWKB`)
stay un-prefixed.

---

## Import / construction

| Function | Signature | Returns |
| --- | --- | --- |
| `ST_3DFromWKB` | `(wkb BLOB)` | `SOLID_3D` |
| `ST_3DFromWKB` | `(wkb BLOB, geometry_properties VARCHAR)` | `SOLID_3D` |
| `ST_3DFromWKB` | `(wkb BLOB, geometry_properties STRUCT)` | `SOLID_3D` |
| `ST_3DTryFromWKB` | same three overloads | `SOLID_3D` or `NULL` |
| `ST_Geom3DFromWKB` | `(wkb BLOB)` | `GEOM_3D` |

### `ST_3DFromWKB` / `ST_3DTryFromWKB`

Builds a `SOLID_3D` from WKB. Accepts `PolyhedralSurface Z`, and `GeometryCollection Z` whose
children are all `PolyhedralSurface Z`.

The optional second argument recovers structure that WKB alone cannot carry — chiefly **shell
grouping**, which is what makes interior shells (cavities) subtract from volume and
`ST_3DNumShells` report the truth. It accepts either JSON text (as `cityjson` emits) **or** a
CityParquet `geometry_properties_lod*` `STRUCT` read straight from Parquet, with no
`to_json(...)` round-trip:

```sql
-- JSON text sidecar (cityjson extension)
SELECT ST_3DVolume(ST_3DFromWKB(geometry, geometry_properties)) FROM feats WHERE geometry IS NOT NULL;

-- native STRUCT (CityParquet file)
SELECT ST_3DVolume(ST_3DFromWKB(geometry_lod2_2, geometry_properties_lod2_2))
FROM read_parquet('building.parquet') WHERE geometry_lod2_2 IS NOT NULL;
```

Without the sidecar a `PolyhedralSurface Z` imports as one solid with **one** shell.

`ST_3DFromWKB` raises on unparseable WKB or unsupported topology; `ST_3DTryFromWKB` returns
`NULL` for that row. Prefer the `TRY` form when scanning real data.

### `ST_Geom3DFromWKB`

Builds a `GEOM_3D` from WKB — the entry point for the distance, serialization, and
general-accessor families. It is named `ST_Geom3DFromWKB` rather than `ST_GeomFromWKB` to
avoid clashing with DuckDB's built-in. **There is no `TRY` variant.**

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
│ POINT Z (84995.2631 446838.844 4.73084539) │
└────────────────────────────────────────────┘
```

---

## Introspection / accessors

| Function | Signature | Returns | Accepts |
| --- | --- | --- | --- |
| `ST_3DBounds` | `(SOLID_3D)` | `STRUCT` (6 fields) | `SOLID_3D` |
| `ST_3DNumSolids` | `(SOLID_3D)` | `BIGINT` | `SOLID_3D` |
| `ST_3DNumShells` | `(SOLID_3D)` | `BIGINT` | `SOLID_3D` |
| `ST_3DNumFaces` | `(SOLID_3D)` | `BIGINT` | `SOLID_3D` |
| `ST_3DZMin` / `ST_3DZMax` | `(SOLID_3D \| GEOM_3D)` | `DOUBLE` | both |
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
│ ST_PolyhedralSurface │ 2   │ 1     │ 3    │
└──────────────────────┴─────┴───────┴──────┘
```

`ST_3DGeometryType` returns one of `ST_Point`, `ST_LineString`, `ST_Polygon`, `ST_MultiPoint`,
`ST_MultiLineString`, `ST_MultiPolygon`, `ST_PolyhedralSurface`. `ST_3DDimension` is the
*topological* dimension: `0` for points, `1` for lines, `2` for surfaces and solids.

> `ST_Geom3DFromWKB` does **not** accept `GeometryCollection Z` — it raises
> `ParseGeomWKB: unsupported geometry class`. Collections of polyhedral surfaces are a
> `SOLID_3D` concern; import them with `ST_3DFromWKB`, which reads them as multi-solids.

### `ST_3DBounds`

```sql
SELECT ST_3DBounds(solid) AS bounds FROM ex;
```
```
{'min_x': 84988.326625, 'min_y': 446832.685, 'min_z': -0.3169973754882822,
 'max_x': 85002.061625, 'max_y': 446844.662, 'max_z': 9.831002624511719}
```

Returns `STRUCT(min_x, min_y, min_z, max_x, max_y, max_z DOUBLE)` from the cached bounding
box. Building height is the Z extent:

```sql
SELECT ROUND(ST_3DZMax(solid) - ST_3DZMin(solid), 2) AS height_m FROM ex;   -- 10.15
```

> `ST_NDims` and `ST_3DHasZ` are constants that never read the payload — they are API-shape
> compatibility with PostGIS, not measurements. `ST_3DX`/`Y`/`Z` raise on anything that is
> not a Point.

---

## Validation

| Function | Signature | Returns |
| --- | --- | --- |
| `ST_3DIsClosed` | `(SOLID_3D)` | `BOOLEAN` |
| `ST_3DIsManifold` | `(SOLID_3D)` | `BOOLEAN` |
| `ST_3DIsOriented` | `(SOLID_3D)` | `BOOLEAN` |
| `ST_3DValidationReport` | `(SOLID_3D)` | `STRUCT` (13 fields) |

Validation runs **at import** and is cached in the payload, so these are cheap reads rather
than recomputation.

- **Closed** — every undirected edge is used exactly twice, in opposing directions.
- **Manifold** — no edge belongs to more than two faces.
- **Oriented** — face winding is consistent within each shell, and interior shells are wound
  opposite the exterior.

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
| `is_valid` | `BOOLEAN` | closed **and** manifold **and** oriented **and** no degenerate faces |
| `is_closed`, `is_manifold`, `is_oriented` | `BOOLEAN` | the individual checks |
| `solid_count`, `shell_count`, `face_count` | `BIGINT` | structure counts |
| `open_edge_count` | `BIGINT` | edges used by only one face |
| `non_manifold_edge_count` | `BIGINT` | edges used by more than two faces |
| `degenerate_face_count` | `BIGINT` | zero-area / collapsed faces |
| `orientation_error_count` | `BIGINT` | inconsistently wound face pairs |
| `code` | `VARCHAR` | `'VALID'` or `'INVALID'` |
| `message` | `VARCHAR` | human-readable summary |

```sql
SELECT ST_3DValidationReport(solid) AS report FROM ex;
```
```
{'is_valid': true, 'is_closed': true, 'is_manifold': true, 'is_oriented': true,
 'solid_count': 1, 'shell_count': 1, 'face_count': 10, 'open_edge_count': 0,
 'non_manifold_edge_count': 0, 'degenerate_face_count': 0, 'orientation_error_count': 0,
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
│ 1116  │ 1098  │ 13           │
└───────┴───────┴──────────────┘
```

The checks **overlap** rather than partition: of those 18 invalid parts, 9 are not closed, 13
are not manifold, 13 are not oriented, and 6 have degenerate faces. A single solid commonly
fails several at once. That is why the report carries per-check counters rather than one flag
— query the individual fields to find out what is actually wrong.

---

## Measurement

| Function | Signature | Returns | Preconditions |
| --- | --- | --- | --- |
| `ST_3DVolume` | `(SOLID_3D)` | `DOUBLE` | closed + manifold + oriented, no degenerate faces — **else raises** |
| `ST_3DSurfaceArea` | `(SOLID_3D)` | `DOUBLE` | no degenerate faces — **else raises** |
| `ST_3DArea` | `(SOLID_3D)` | `DOUBLE` | alias of `ST_3DSurfaceArea` |
| `ST_3DFootprintArea` | `(SOLID_3D \| GEOM_3D)` | `DOUBLE` | none |
| `ST_3DPerimeter` | `(SOLID_3D)` | `DOUBLE` | none |
| `ST_3DLength` | `(GEOM_3D)` | `DOUBLE` | none |

All measurements are **Cartesian in the input units** — there is no stored CRS. Reproject with
`ST_3DTransform` first if the source units are not metric.

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
- **`ST_3DPerimeter`** measures *boundary* edges — those used by exactly one face. A **closed
  solid therefore returns `0`**; the function is meaningful for open shells.
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

---

## Distance / relationships

All take **`GEOM_3D`** — build it with `ST_Geom3DFromWKB`.

| Function | Signature | Returns |
| --- | --- | --- |
| `ST_3DDistance` | `(GEOM_3D, GEOM_3D)` | `DOUBLE` |
| `ST_3DMaxDistance` | `(GEOM_3D, GEOM_3D)` | `DOUBLE` |
| `ST_3DDWithin` | `(GEOM_3D, GEOM_3D, dist DOUBLE)` | `BOOLEAN` |
| `ST_3DDFullyWithin` | `(GEOM_3D, GEOM_3D, dist DOUBLE)` | `BOOLEAN` |
| `ST_3DIntersects` | `(GEOM_3D, GEOM_3D)` | `BOOLEAN` |
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
one geometry lies entirely inside a buffer of the other. `ST_3DIntersects` treats touching
geometries as intersecting.

Find neighbours of a reference building:

```sql
WITH ref AS (SELECT g FROM parts WHERE id = 'NL.IMBAG.Pand.0503100000018426-0')
SELECT p.id FROM parts p, ref
WHERE p.id <> 'NL.IMBAG.Pand.0503100000018426-0'
  AND ST_3DDWithin(p.g, ref.g, 5.0);
```

---

## Transform / construct

| Function | Signature | Returns |
| --- | --- | --- |
| `ST_3DTranslate` | `(SOLID_3D \| GEOM_3D, dx, dy, dz DOUBLE)` | same type as input |
| `ST_3DScale` | `(SOLID_3D \| GEOM_3D, sx, sy, sz DOUBLE)` | same type as input |
| `ST_3DRotateX/Y/Z` | `(SOLID_3D \| GEOM_3D, radians DOUBLE)` | same type as input |
| `ST_3DTransform` | `(SOLID_3D \| GEOM_3D, src, tgt)` | same type as input |
| `ST_3DExtrude` | `(GEOM_3D polygon, height DOUBLE)` | `SOLID_3D` |
| `ST_MakeSolid` | `(GEOM_3D)` | `SOLID_3D` |
| `ST_3DCentroid` | `(GEOM_3D)` | `GEOM_3D` (Point) |
| `ST_3DConvexHull` | `(GEOM_3D)` | `GEOM_3D` |
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
│ 881.31     │ 7050.52   │ 881.31  │
└────────────┴───────────┴─────────┘
```

Translation and rotation are rigid motions and preserve volume; scaling by 2 in each axis
multiplies it by 8, as expected. Rotations follow the PostGIS convention: **right-handed,
counter-clockwise, in radians**.

### `ST_3DTransform` — CRS reprojection

```
ST_3DTransform(geom, source_srid INTEGER, target_srid INTEGER) → same type
ST_3DTransform(geom, source_crs VARCHAR, target_crs VARCHAR)   → same type
```

PROJ-backed reprojection with **PostGIS's horizontal-only semantics: X and Y are reprojected,
Z is passed through unchanged.** There is no vertical datum or geoid transformation.

The string form takes an authority code (`'EPSG:28992'`) or a WKT2 CRS string; the integer
form is shorthand for `EPSG:<n>`. PROJ **pipeline** strings (`'+proj=pipeline …'`) are *not*
accepted. Axis order is normalised to easting/northing, so `EPSG:4326` behaves as
**(lon, lat)** — the GIS convention, not the authority's declared order.

No SRID is stored in the payload, so **both** CRSs must be given on every call.

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
SELECT ST_3DNumFaces(ST_3DExtrude(ST_3DConvexHull(g), 10.0))            AS prism_faces,
       ROUND(ST_3DVolume(ST_3DExtrude(ST_3DConvexHull(g), 10.0)), 2)    AS prism_vol,
       ROUND(ST_3DFootprintArea(ST_3DConvexHull(g)) * 10.0, 2)          AS footprint_x_height
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

### `ST_3DCentroid`, `ST_3DConvexHull`, `ST_Force3D`

- **`ST_3DCentroid`** — area-weighted for surfaces and solids, length-weighted for lines,
  vertex average for points. Returns a `Point Z`.
- **`ST_3DConvexHull`** — a **2D** hull over the XY projection of all vertices, returned at
  the input's **minimum Z**. It is a footprint hull, not a true 3D hull (which would need a
  CGAL/SFCGAL backend). Degenerate inputs return a `LineString Z` or `Point Z`.
- **`ST_Force3D`** — currently an identity round-trip, since `GEOM_3D` already stores XYZ.
  Reserved for future 2D inputs.

---

## Function index

55 public functions.

| Category | Functions |
| --- | --- |
| **Import** | `ST_3DFromWKB`, `ST_3DTryFromWKB`, `ST_Geom3DFromWKB` |
| **Export** | `ST_3DAsWKB`, `ST_3DAsText`, `ST_3DAsGeoJSON`, `ST_3DAsBinary` |
| **Introspection** | `ST_3DBounds`, `ST_3DNumSolids`, `ST_3DNumShells`, `ST_3DNumFaces`, `ST_3DZMin`, `ST_3DZMax`, `ST_NDims`, `ST_3DHasZ`, `ST_CoordDim`, `ST_3DGeometryType`, `ST_3DDimension`, `ST_3DNumGeometries`, `ST_3DX`, `ST_3DY`, `ST_3DZ`, `ST_IsPlanar` |
| **Validation** | `ST_3DIsClosed`, `ST_3DIsManifold`, `ST_3DIsOriented`, `ST_3DValidationReport` |
| **Measurement** | `ST_3DVolume`, `ST_3DSurfaceArea`, `ST_3DArea`, `ST_3DFootprintArea`, `ST_3DPerimeter`, `ST_3DLength` |
| **Distance** | `ST_3DDistance`, `ST_3DMaxDistance`, `ST_3DDWithin`, `ST_3DDFullyWithin`, `ST_3DIntersects`, `ST_3DClosestPoint`, `ST_3DShortestLine` |
| **Transform / construct** | `ST_3DTranslate`, `ST_3DScale`, `ST_3DRotateX`, `ST_3DRotateY`, `ST_3DRotateZ`, `ST_3DTransform`, `ST_3DExtrude`, `ST_MakeSolid`, `ST_3DCentroid`, `ST_3DConvexHull`, `ST_Force3D` |

**Not implemented.** These PostGIS names appear in comparison tables but are **not**
registered: `ST_3DIsValid` (validity is a field of `ST_3DValidationReport`), `ST_3DUnion` /
`ST_3DIntersection` / `ST_3DDifference`, `ST_3DLongestLine`, `ST_3DExtent`, `ST_Affine`,
`ST_SRID` / `ST_SetSRID`, `ST_AsGML` / `ST_AsKML` / `ST_AsX3D`, `ST_HasM` / `ST_M`,
`ST_Boundary`, `ST_PointOnSurface`, `ST_Tesselate`, `ST_StraightSkeleton`,
`ST_ApproximateMedialAxis`. The boolean and hull operations await a decision on a
CGAL/SFCGAL backend — see [DESIGN_DOC.md §11](./DESIGN_DOC.md#11-roadmap).

---

## Appendix: test fixtures

The `ST_AsWKB*` family (`ST_AsWKBPolyhedralTetra`, `ST_AsWKBOpenTetra`, `ST_AsWKBHollowCube`,
`ST_AsWKBMultiCube`, `ST_AsWKBPointZ`, `ST_AsWKBLineZ`, `ST_AsWKBMultiLineZ`,
`ST_AsWKBPolygonZ`, `ST_AsWKBWarpedPolygonZ`, `ST_AsWKBMultiPointZ`, `ST_AsWKBMultiPolygonZ`)
generates small WKB fixtures for the test suite. `ST_AsWKBMultiCube` has a second overload,
`ST_AsWKBMultiCube(separation DOUBLE)`, which places the two cubes `separation` apart on every
axis; total volume stays 16 at any separation, which is what the conditioning regression in
`test/sql/st_3d_multisolid.test` pins.

**These are not public API.** They are registered only when the `THREE_D_TEST_FIXTURES`
environment variable is set. The `Makefile` exports it, so every `make` target has them; a
directly-invoked `build/release/duckdb` does not, and SQL test files declaring
`require-env THREE_D_TEST_FIXTURES` are silently skipped without it.

```sh
THREE_D_TEST_FIXTURES=1 ./build/release/duckdb -unsigned \
  -c "SELECT ST_3DVolume(ST_3DFromWKB(ST_AsWKBHollowCube()));"   -- 56.0
```
