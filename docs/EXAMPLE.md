# Using `duckdb-3d` — a worked example

This is a hands-on walkthrough of the `three_d` extension against **real 3D city
models**: the [3DBAG](https://3dbag.nl) reconstruction of Delft, streamed straight
from a remote server as CityJSONSeq and turned into queryable solids with the
[`cityjson`](https://github.com/cityjson/duckdb-cityjson) extension.

For the formal contract of every function, see [FUNCTIONS.md](./FUNCTIONS.md). For the
extension-composition mechanics and troubleshooting, see
[CITYJSON_INTEROP.md](./CITYJSON_INTEROP.md).

## How the two extensions compose

`three_d` never parses CityJSON itself. The division of labour is:

- **`cityjson`** reads CityJSON / CityJSONSeq and, in per-LoD mode (`lod => '...'`),
  emits one `geometry_lod<X> BLOB` (WKB) column per LoD present, each paired with a
  sidecar `geometry_properties_lod<X> STRUCT`.
- **`three_d`** consumes that pair through
  `ST_3DFromWKB(geometry_lod<X>, geometry_properties_lod<X>)` and gives you a `SOLID_3D`
  you can validate, measure, and transform.

```
CityJSON ──cityjson──▶ (geometry_lod2_2 BLOB, geometry_properties_lod2_2 STRUCT) ──three_d──▶ SOLID_3D / GEOM_3D
```

The suffix is the **normalised** LoD, so `lod => '2.2'` gives `_lod2_2` and `lod => '2'`
gives `_lod2_0` — not `_lod2`.

## Setup

Build both extensions from source. They pin the same DuckDB core, so the two load into
one process without an ABI complaint:

```sh
GEN=ninja make                                   # ./build/release/duckdb, three_d linked in
(cd ../duckdb-cityjson && GEN=ninja make release)
```

`cityjson` is loaded **by path** — the published community build is a different, older
extension that still emits a flat `geometry` column, and none of the SQL below binds
against it:

```sql
LOAD '../duckdb-cityjson/build/release/extension/cityjson/cityjson.duckdb_extension';
LOAD three_d;
```

> On macOS debug builds, add `ASAN_OPTIONS=detect_container_overflow=0` in front of the
> shell command when reading remote files — see the caveats in
> [CITYJSON_INTEROP.md](./CITYJSON_INTEROP.md).

## 1. A local sanity check

Before touching the network, confirm the pipeline on the bundled unit-cube fixture:

```sql
SELECT id,
       ST_3DNumFaces(solid)              AS faces,
       ST_3DIsClosed(solid)             AS closed,
       ROUND(ST_3DSurfaceArea(solid), 3) AS area,
       ROUND(ST_3DVolume(solid), 3)      AS volume
FROM (
  SELECT id, ST_3DFromWKB(geometry_lod2_2, geometry_properties_lod2_2) AS solid
  FROM read_cityjson('test/data/unit_cube.city.json', lod => '2.2')
  WHERE geometry_lod2_2 IS NOT NULL
);
```

```
┌──────┬───────┬────────┬───────┬────────┐
│  id  │ faces │ closed │ area  │ volume │
├──────┼───────┼────────┼───────┼────────┤
│ cube │   6   │ true   │  6.0  │  1.0   │
└──────┴───────┴────────┴───────┴────────┘
```

## 2. Streaming real buildings from a remote server

The 3DBAG Delft tile is published as CityJSONSeq. Read it directly by URL, in LoD 2.2:

```sql
CREATE TABLE feats AS
SELECT id, parents, children,
       geometry_lod2_2            AS geometry,
       geometry_properties_lod2_2 AS geometry_properties,
       b3_volume_lod22, b3_opp_grond, b3_h_maaiveld     -- 3DBAG ground-truth attributes
FROM read_cityjsonseq(
    'https://cityjson.open3d.city/cityjsonseq/delft.city.jsonl',
    lod => '2.2');
```

**Important structural detail.** In 3DBAG the geometry lives on `BuildingPart`
features, while the descriptive attributes (`b3_volume_lod22`, height percentiles,
areas) live on the parent `Building`. When `cityjson` flattens the sequence, the
geometry row and the attribute row are *different* rows linked by `parents` /
`children`. Join a part to its parent to line geometry up with ground truth:

```sql
CREATE TABLE parts AS
SELECT p.id,
       ST_3DTryFromWKB(p.geometry, p.geometry_properties) AS solid,
       b.b3_volume_lod22,
       b.b3_opp_grond,
       len(b.children) AS n_parts
FROM feats p
JOIN feats b ON b.id = p.parents[1]
WHERE p.geometry IS NOT NULL;
```

Aliasing the two per-LoD columns to `geometry` / `geometry_properties` once, as above, is
worth doing when a query mentions them repeatedly.

Use `ST_3DTryFromWKB` (not `ST_3DFromWKB`) when exploring real data: it returns `NULL`
on an unsupported geometry instead of aborting the whole query.

## 3. Measurements that matter for buildings

```sql
SELECT id,
       ROUND(ST_3DVolume(solid), 1)            AS volume_m3,
       ROUND(ST_3DFootprintArea(solid), 1)                AS footprint_m2,
       ROUND(ST_3DZMax(solid) - ST_3DZMin(solid), 2) AS height_m,   -- roof − ground
       ST_3DIsClosed(solid)                    AS closed
FROM parts
WHERE ST_3DValidationReport(solid).is_valid
LIMIT 5;
```

Key expressions:

| Metric | Expression | Notes |
| --- | --- | --- |
| Enclosed volume | `ST_3DVolume(solid)` | Requires a valid (closed + manifold + oriented) solid. |
| Footprint area | `ST_3DFootprintArea(solid)` | 2D XY projection; the building's ground area. |
| Building height | `ST_3DZMax(solid) - ST_3DZMin(solid)` | From the cached bounding box. |
| 3D surface area | `ST_3DArea(solid)` / `ST_3DSurfaceArea(solid)` | Total area of all faces. |

## 4. Validate before you measure

`ST_3DVolume` **raises** on a non-manifold or open solid — real reconstructions always
contain a few. Guard measurements with the validation report:

```sql
-- How healthy is the tile?
SELECT count(*)                                                   AS parts,
       count(*) FILTER (WHERE ST_3DValidationReport(solid).is_valid) AS valid,
       count(*) FILTER (WHERE ST_3DIsClosed(solid))               AS closed
FROM parts;
```

The report is a struct — inspect any building that fails:

```sql
SELECT id, ST_3DValidationReport(solid) AS report
FROM parts
WHERE NOT ST_3DValidationReport(solid).is_valid
LIMIT 3;
-- report = {is_valid, is_closed, is_manifold, is_oriented, solid_count, shell_count,
--           face_count, open_edge_count, non_manifold_edge_count, degenerate_face_count,
--           orientation_error_count, code, message}
```

Compute total built volume over only the valid solids:

```sql
SELECT ROUND(SUM(ST_3DVolume(solid)), 0) AS total_volume_m3
FROM parts
WHERE ST_3DValidationReport(solid).is_valid;
```

## 5. Cross-checking against 3DBAG's own numbers

3DBAG ships each building's own computed volume (`b3_volume_lod22`) and ground-surface
area (`b3_opp_grond`). `three_d`'s independent kernel agrees with them almost exactly —
for single-part, valid buildings the median volume error is 0.017 %:

```sql
SELECT ROUND(median(
         abs(ST_3DVolume(solid) - b3_volume_lod22) / b3_volume_lod22
       ) * 100, 3) AS median_volume_error_pct
FROM parts
WHERE n_parts = 1
  AND ST_3DValidationReport(solid).is_valid
  AND b3_volume_lod22 > 0;
-- 0.017 %
```

This double-checks both the data and the extension. The automated version of this check
lives in [`test/sql/cityjson_delft_remote.test`](../test/sql/cityjson_delft_remote.test).

## 6. Proximity queries with `GEOM_3D`

Distance and relationship functions operate on the general geometry type `GEOM_3D`, which
you build from WKB with `ST_Geom3DFromWKB`. Minimum 3D distance between two named
buildings:

```sql
SELECT ROUND(ST_3DDistance(
    ST_Geom3DFromWKB((SELECT geometry FROM feats WHERE id = 'NL.IMBAG.Pand.0503100000012869-0')),
    ST_Geom3DFromWKB((SELECT geometry FROM feats WHERE id = 'NL.IMBAG.Pand.0503100000016459-0'))
), 1) AS gap_m;                                     -- ≈ 1033.7
```

Find every building within 5 m of a reference building (`ST_3DDWithin` short-circuits on
distance):

```sql
WITH ref AS (
  SELECT ST_Geom3DFromWKB(geometry) AS g
  FROM feats WHERE id = 'NL.IMBAG.Pand.0503100000012869-0'
)
SELECT p.id
FROM feats p, ref
WHERE p.geometry IS NOT NULL
  AND ST_3DDWithin(ST_Geom3DFromWKB(p.geometry), ref.g, 5.0);
```

Related functions on `GEOM_3D`: `ST_3DMaxDistance`, `ST_3DDFullyWithin`,
`ST_3DIntersects`, `ST_3DClosestPoint`, `ST_3DShortestLine`.

## 7. Inspecting and exporting geometry

`GEOM_3D` carries accessors and serializers for debugging and interchange:

```sql
SELECT ST_3DGeometryType(g)        AS gtype,        -- ST_PolyhedralSurface
       ST_NDims(g)               AS dims,         -- 3
       ST_3DAsText(ST_3DCentroid(g)) AS centroid     -- POINT Z (84595.382 446461.183 1.82866198)
FROM (SELECT ST_Geom3DFromWKB(geometry) AS g
      FROM feats WHERE id = 'NL.IMBAG.Pand.0503100000012869-0');
```

- `ST_3DAsText(geom)` → ISO WKT (with Z)
- `ST_3DAsGeoJSON(geom)` → GeoJSON (a `PolyhedralSurface` is emitted as a `MultiPolygon`)
- `ST_3DAsBinary(geom)` → OGC/ISO WKB
- `ST_3DAsWKB(solid)` → WKB from the canonical `SOLID_3D` model

## 8. Transformations and construction

Placement and geometric edits are per-vertex and preserve topology:

```sql
-- shift a building 100 m east, 50 m north
SELECT ST_3DTranslate(solid, 100, 50, 0) FROM parts LIMIT 1;
```

Available transforms: `ST_3DTranslate`, `ST_3DScale`, `ST_3DRotateX/Y/Z`, `ST_Force3D`,
`ST_3DConvexHull` (2D XY hull), `ST_IsPlanar`.

Construction:

- `ST_3DExtrude(footprint_polygon, height)` — extrude a 2D footprint (`GEOM_3D`
  `Polygon Z`) into a closed LoD1 prism `SOLID_3D`. Extruding a 4 m × 3 m footprint by
  3 m yields a closed 6-face box of volume 36.
- `ST_MakeSolid(surface)` — promote a closed/manifold/oriented `PolyhedralSurface` to a
  `SOLID_3D` (no silent repair; raises if it is not solid-eligible).

## Gotchas checklist

- **Always pass `lod => '...'`** to `read_cityjson` / `read_cityjsonseq`. Without it,
  `cityjson` emits per-LoD `geom_lod*` structs that `ST_3DFromWKB` cannot consume.
- **The LoD suffix is normalised.** `lod => '2'` yields `geometry_lod2_0`, and the object
  may carry several LoDs at once — `DESCRIBE` the reader to see which columns exist.
- **Filter `WHERE geometry_lod<X> IS NOT NULL`** upstream — objects lacking the requested
  LoD have a NULL geometry.
- **Load `cityjson` by path from the sibling build.** The community-published extension
  is older and uses the flat `geometry` / `geometry_properties VARCHAR` shape; this
  document targets only the current one.
- **Attributes are on the parent `Building`**, geometry is on the `BuildingPart`; join
  via `parents` / `children` (§2).
- **Validate before `ST_3DVolume` / `ST_3DSurfaceArea`** — they raise on invalid solids.
  Use `ST_3DTryFromWKB` and `ST_3DValidationReport` while exploring.
- **`SOLID_3D`-only vs `GEOM_3D`-only**: solid metrics (`ST_3DVolume`, `ST_3DIsClosed`,
  `ST_3DBounds`) take the solid payload; distance/serialization accessors take `GEOM_3D`
  (build it with `ST_Geom3DFromWKB`). Many accessors (`ST_NDims`, `ST_3DZMin`, `ST_3DZMax`,
  `ST_3DTranslate`, …) accept both.
```
