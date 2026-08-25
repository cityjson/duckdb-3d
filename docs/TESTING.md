# Manual testing notebook — every public function against real data

A copy-pasteable SQL walkthrough that exercises **all 45 public `duckdb-3d` functions**
against real 3D city models: local CityJSON, remote CityJSONSeq (3DBAG Delft and
Helsinki) and an on-disk **CityParquet** package. Every cell below was executed and its printed output is the real output,
not an illustration.

This complements the other docs rather than repeating them:

| Doc | Purpose |
| --- | --- |
| [FUNCTIONS.md](./FUNCTIONS.md) | The formal contract of each function |
| [EXAMPLE.md](./EXAMPLE.md) | A narrative task-oriented tour of 3DBAG Delft |
| [CITYJSON_INTEROP.md](./CITYJSON_INTEROP.md) | Composition mechanics and troubleshooting |
| **TESTING.md** (this file) | Breadth: prove every function runs on real data, and record what surprised us |

Read [Quirks and known gaps](#quirks-and-known-gaps) before trusting any cell —
several outputs diverge from the naive expectation, and one of them was a real bug that
this walkthrough found and fixed.

---

## Setup

Two extensions are needed: `three_d` (this repo) and `cityjson` (the sibling
[`duckdb-cityjson`](https://github.com/cityjson/duckdb-cityjson) repo) for the readers.
`spatial` and `json` are used by a handful of cells. The extension build ships with
`autoload_known_extensions` off, so `LOAD` them explicitly where a cell needs them.

```sh
GEN=ninja make release        # builds ./build/release/duckdb with three_d linked in
```

### Which `cityjson` build this notebook uses — and why it matters

This walkthrough was run against a **locally built** `cityjson`, not the community
download. Both repos pin the same DuckDB core (`v1.5.4`), so the two locally built
extensions load into one process without any ABI complaint:

```sh
cd duckdb-cityjson && GEN=ninja make release     # sibling repo, built once
```

```sh
THREE_D_TEST_FIXTURES=1 ./build/release/duckdb -unsigned
```

```sql
LOAD '../duckdb-cityjson/build/release/extension/cityjson/cityjson.duckdb_extension';
LOAD three_d;
```

```
┌────────────────┬───────────────────┬───────────────────┐
│ extension_name │ extension_version │   install_mode    │
├────────────────┼───────────────────┼───────────────────┤
│ cityjson       │ aa64174           │ REPOSITORY        │
│ spatial        │ 28db190           │ REPOSITORY        │
│ three_d        │ 5c25f21           │ STATICALLY_LINKED │
└────────────────┴───────────────────┴───────────────────┘
```

**This choice changes the SQL you write.** The current `cityjson` and the community build
that `INSTALL cityjson FROM community` still serves (`d511bdb`) expose *different column
names*:

| | community `d511bdb` | local build (used here) |
| --- | --- | --- |
| Geometry column | `geometry` `BLOB` | `geometry_lod2_2` `BLOB` (one per LoD present) |
| Sidecar | `geometry_properties` `VARCHAR` (JSON) | `geometry_properties_lod2_2` `STRUCT` |

`ST_3DFromWKB` accepts both sidecar forms — the `VARCHAR` and `STRUCT` overloads are
[documented](./FUNCTIONS.md#st_3dfromwkb--st_3dtryfromwkb) and both are covered by the
automated suite — but the *column names* differ, so cells written for one build do not
bind against the other. This repo now targets the **local build only**: `docs/EXAMPLE.md`,
`docs/CITYJSON_INTEROP.md`, and the gated `test/sql/cityjson_*.test` files all use the
per-LoD naming, and `make test_full` stages the local build rather than downloading the
community one. Support for the flat shape was dropped rather than kept on a compatibility
branch.

One more naming rule: the suffix is the *normalised* LoD, so `lod => '2'` produces
`geometry_lod2_0`, not `geometry_lod2`.

---

# Part A — Local CityJSON

## 1 — Sanity check on the bundled unit cube

The cheapest end-to-end proof that both extensions are talking to each other. Covers
`ST_3DFromWKB`, the four counters, and the three validation predicates.

```sql
SELECT id,
       ST_3DNumSolids(solid)  AS solids,
       ST_3DNumShells(solid)  AS shells,
       ST_3DNumFaces(solid)   AS faces,
       ST_3DIsClosed(solid)   AS closed,
       ST_3DIsManifold(solid) AS manifold,
       ST_3DIsOriented(solid) AS oriented,
       ROUND(ST_3DSurfaceArea(solid), 3) AS area,
       ROUND(ST_3DVolume(solid), 3)      AS volume
FROM (
  SELECT id, ST_3DFromWKB(geometry_lod2_2, geometry_properties_lod2_2) AS solid
  FROM read_cityjson('test/data/unit_cube.city.json', lod => '2.2')
  WHERE geometry_lod2_2 IS NOT NULL
);
```

```
┌──────┬────────┬────────┬───────┬────────┬──────────┬──────────┬──────┬────────┐
│  id  │ solids │ shells │ faces │ closed │ manifold │ oriented │ area │ volume │
├──────┼────────┼────────┼───────┼────────┼──────────┼──────────┼──────┼────────┤
│ cube │ 1      │ 1      │ 6     │ true   │ true     │ true     │ 6.0  │ 1.0    │
└──────┴────────┴────────┴───────┴────────┴──────────┴──────────┴──────┴────────┘
```

## 2 — Hollow solid: interior shells survive the WKB round trip

A CityJSON `Solid` flattens all its shells into one WKB `PolyhedralSurface`, losing the
exterior/interior split. `shells` in the sidecar is what puts it back, so the cavity
subtracts (64 − 8 = 56) instead of being ignored.

```sql
SELECT geometry_properties_lod2_0 AS props
FROM read_cityjson('test/data/hollow_solid.city.json', lod => '2')
WHERE geometry_lod2_0 IS NOT NULL;

SELECT ST_3DNumShells(s)        AS shells,
       ST_3DIsClosed(s)         AS closed,
       ROUND(ST_3DVolume(s), 6) AS volume,
       ROUND(ST_3DSurfaceArea(s), 6) AS area
FROM (
  SELECT ST_3DFromWKB(geometry_lod2_0, geometry_properties_lod2_0) AS s
  FROM read_cityjson('test/data/hollow_solid.city.json', lod => '2')
  WHERE geometry_lod2_0 IS NOT NULL
);
```

```
┌───────────────────────────────────────────────────────────────────────────────┐
│                                     props                                     │
├───────────────────────────────────────────────────────────────────────────────┤
│ {'type': Solid, 'surfaces': NULL, 'face_semantics': NULL, 'shells': [[6, 6]]} │
└───────────────────────────────────────────────────────────────────────────────┘
┌────────┬────────┬────────┬───────┐
│ shells │ closed │ volume │ area  │
├────────┼────────┼────────┼───────┤
│ 2      │ true   │ 56.0   │ 120.0 │
└────────┴────────┴────────┴───────┘
```

Note `shells` arrives as `[[6, 6]]` — the CityParquet `List<List<Int32>>` shape, with an
extra wrapping level compared to the flat `[6,6]` the community build emits. Both are
accepted; see [CITYJSON_INTEROP.md](./CITYJSON_INTEROP.md) and §21.

Surface area is **not** shell-aware: 96 (outer) + 24 (cavity walls) = 120. Only volume
distinguishes shell roles.

## 3 — `MultiSolid` and `CompositeSolid` import per-solid

Both import cleanly and `ST_3DNumSolids` recovers the partition from `shells`.

```sql
SELECT geometry_properties_lod1_0.type AS cj_type,
       ST_3DNumSolids(ST_3DFromWKB(geometry_lod1_0, geometry_properties_lod1_0)) AS solids,
       ST_3DNumShells(ST_3DFromWKB(geometry_lod1_0, geometry_properties_lod1_0)) AS shells,
       ROUND(ST_3DVolume(ST_3DFromWKB(geometry_lod1_0, geometry_properties_lod1_0)), 6) AS volume
FROM read_cityjson('test/data/multisolid.city.json', lod => '1')
WHERE geometry_lod1_0 IS NOT NULL
UNION ALL
SELECT geometry_properties_lod1_0.type,
       ST_3DNumSolids(ST_3DFromWKB(geometry_lod1_0, geometry_properties_lod1_0)),
       ST_3DNumShells(ST_3DFromWKB(geometry_lod1_0, geometry_properties_lod1_0)),
       ROUND(ST_3DVolume(ST_3DFromWKB(geometry_lod1_0, geometry_properties_lod1_0)), 6)
FROM read_cityjson('test/data/compositesolid.city.json', lod => '1')
WHERE geometry_lod1_0 IS NOT NULL;
```

```
┌────────────────┬────────┬────────┬────────┐
│    cj_type     │ solids │ shells │ volume │
├────────────────┼────────┼────────┼────────┤
│ MultiSolid     │ 2      │ 2      │ 2.0    │
│ CompositeSolid │ 2      │ 2      │ 2.0    │
└────────────────┴────────┴────────┴────────┘
```

Volumes **sum** across members (two unit cubes = 2.0); they do not cancel even if one
member is globally reversed.

## 4 — A real LoD3 file is surface-family, not solid-family

`lod3_railway.city.json` is a real CityGML-derived LoD3 model. At LoD3 it contains **no
solids at all** — every geometry is a `MultiSurface` or `CompositeSurface`.

```sql
SELECT geometry_properties_lod3_0.type AS cj_type,
       count(*) AS n,
       count(ST_3DTryFromWKB(geometry_lod3_0, geometry_properties_lod3_0)) AS as_solid
FROM read_cityjson('../cityparquet-rs/tests/fixtures/lod3_railway.city.json', lod => '3')
WHERE geometry_lod3_0 IS NOT NULL
GROUP BY 1 ORDER BY 1;
```

```
┌──────────────────┬─────┬──────────┐
│     cj_type      │  n  │ as_solid │
├──────────────────┼─────┼──────────┤
│ CompositeSurface │ 1   │ 0        │
│ MultiSurface     │ 104 │ 0        │
└──────────────────┴─────┴──────────┘
```

`ST_3DTryFromWKB` returns `NULL` for every row. The strict form raises, and the message
names the WKB type code rather than the CityJSON type:

```sql
SELECT ST_3DFromWKB(geometry_lod3_0, geometry_properties_lod3_0)
FROM read_cityjson('../cityparquet-rs/tests/fixtures/lod3_railway.city.json', lod => '3')
WHERE geometry_lod3_0 IS NOT NULL LIMIT 1;
```

```
Invalid Error: Unsupported WKB geometry type for SOLID_3D import: type code 1006
```

`1006` is `MultiPolygon Z` — `SOLID_3D` only ingests `PolyhedralSurface Z`. Surface-family
geometry belongs in `GEOM_3D` (next cell).

## 5 — `GEOM_3D` accessors on the real LoD3 model

Covers `ST_Geom3DFromWKB`, `ST_3DGeometryType`, `ST_3DNumGeometries`, `ST_3DDimension`,
`ST_NDims`, `ST_CoordDim`, `ST_3DHasZ`, `ST_IsPlanar`, `ST_3DZMin`/`ST_3DZMax`.

```sql
SELECT id, object_type,
       ST_3DGeometryType(g)  AS gtype,
       ST_3DNumGeometries(g) AS n_patches,
       ST_3DDimension(g)     AS dim,
       ST_NDims(g)           AS ndims,
       ST_CoordDim(g)        AS coorddim,
       ST_3DHasZ(g)          AS has_z,
       ST_IsPlanar(g)        AS planar,
       ROUND(ST_3DFootprintArea(g), 2) AS footprint,
       ROUND(ST_3DZMin(g), 2) AS zmin,
       ROUND(ST_3DZMax(g), 2) AS zmax
FROM (
  SELECT id, object_type, ST_Geom3DFromWKB(geometry_lod3_0) AS g
  FROM read_cityjson('../cityparquet-rs/tests/fixtures/lod3_railway.city.json', lod => '3')
  WHERE geometry_lod3_0 IS NOT NULL
    AND object_type IN ('Railway', 'Bridge')
) ORDER BY object_type, id LIMIT 6;
```

```
┌────────────────────────────┬─────────────┬─────────────────┬───────────┬─────┬───────┬──────────┬───────┬────────┬───────────┬──────┬──────┐
│             id             │ object_type │      gtype      │ n_patches │ dim │ ndims │ coorddim │ has_z │ planar │ footprint │ zmin │ zmax │
├────────────────────────────┼─────────────┼─────────────────┼───────────┼─────┼───────┼──────────┼───────┼────────┼───────────┼──────┼──────┤
│ GMLID_BUI100628_817_8083   │ Bridge      │ ST_MultiPolygon │ 30        │ 2   │ 3     │ 3        │ true  │ false  │ 0.7       │ 8.59 │ 8.7  │
│ GMLID_BUI205585_1385_1373  │ Bridge      │ ST_MultiPolygon │ 30        │ 2   │ 3     │ 3        │ true  │ false  │ 0.72      │ 8.42 │ 8.54 │
│ GMLID_BUI30683_572_6686    │ Bridge      │ ST_MultiPolygon │ 503       │ 2   │ 3     │ 3        │ true  │ false  │ 1.95      │ 8.33 │ 8.91 │
│ GMLID_BUI51891_765_738     │ Bridge      │ ST_MultiPolygon │ 626       │ 2   │ 3     │ 3        │ true  │ false  │ 1.37      │ 8.55 │ 8.86 │
│ GMLID_0632464_192141_968   │ Railway     │ ST_MultiPolygon │ 2638      │ 2   │ 3     │ 3        │ true  │ false  │ 3.07      │ 8.0  │ 8.08 │
│ GMLID_10415082_319158_1266 │ Railway     │ ST_MultiPolygon │ 6568      │ 2   │ 3     │ 3        │ true  │ false  │ 6.05      │ 8.0  │ 8.02 │
└────────────────────────────┴─────────────┴─────────────────┴───────────┴─────┴───────┴──────────┴───────┴────────┴───────────┴──────┴──────┘
```

`ST_3DDimension` is `2` (surfaces) while `ST_NDims`/`ST_CoordDim` are `3` (coordinate
dimension) — different questions, easily confused. The footprints look implausibly small
for railway infrastructure because **this fixture's coordinates are not metric**: its
`geographicalExtent` is `[0.56, 0.64, 7.579, 12.64, 7.68, 9.103]`, a ~12-unit-wide model.
Measurements are Cartesian in whatever units the input carries.

## 6 — Point accessors and serialization on a centroid

`ST_3DCentroid`, `ST_3DX`/`ST_3DY`/`ST_3DZ`, `ST_3DAsText`, and `ST_3DLength`.

```sql
SELECT id,
       ST_3DAsText(ST_3DCentroid(g)) AS centroid,
       ROUND(ST_3DX(ST_3DCentroid(g)), 3) AS cx,
       ROUND(ST_3DY(ST_3DCentroid(g)), 3) AS cy,
       ROUND(ST_3DZ(ST_3DCentroid(g)), 3) AS cz,
       ROUND(ST_3DLength(g), 3) AS len3d
FROM (
  SELECT id, ST_Geom3DFromWKB(geometry_lod3_0) AS g
  FROM read_cityjson('../cityparquet-rs/tests/fixtures/lod3_railway.city.json', lod => '3')
  WHERE object_type = 'Railway' AND geometry_lod3_0 IS NOT NULL LIMIT 2
);
```

```
┌────────────────────────────┬────────────────────────────────────────────┬───────┬───────┬───────┬───────┐
│             id             │                  centroid                  │  cx   │  cy   │  cz   │ len3d │
├────────────────────────────┼────────────────────────────────────────────┼───────┼───────┼───────┼───────┤
│ GMLID_0632464_192141_968   │ POINT Z (4.19173326 1.18174351 8.01251848) │ 4.192 │ 1.182 │ 8.013 │ 0.0   │
│ GMLID_10415082_319158_1266 │ POINT Z (6.7775388 2.45251156 8.0117759)   │ 6.778 │ 2.453 │ 8.012 │ 0.0   │
└────────────────────────────┴────────────────────────────────────────────┴───────┴───────┴───────┴───────┘
```

`ST_3DLength` is `0.0`, not an error: it is defined for `LineString`/`MultiLineString`
only and returns zero for every other class. A non-zero one appears in §12.

---

# Part B — Remote CityJSONSeq (3DBAG Delft)

## 7 — Stream the tile

3DBAG puts geometry on `BuildingPart` and attributes on the parent `Building`, so the
row that has a solid is never the row that has `b3_volume_lod22`.

```sql
CREATE TABLE feats AS
SELECT id, object_type, parents, children,
       geometry_lod2_2 AS geom, geometry_properties_lod2_2 AS props,
       b3_volume_lod22, b3_opp_grond, b3_h_maaiveld
FROM read_cityjsonseq('https://cityjson.open3d.city/cityjsonseq/delft.city.jsonl',
                      lod => '2.2');

SELECT object_type, count(*) AS n, count(geom) AS with_geom
FROM feats GROUP BY 1 ORDER BY 1;
```

```
┌──────────────┬──────┬───────────┐
│ object_type  │  n   │ with_geom │
├──────────────┼──────┼───────────┤
│ Building     │ 1115 │ 0         │
│ BuildingPart │ 1116 │ 1116      │
└──────────────┴──────┴───────────┘
```

## 8 — Join parts to parents, import with the `TRY` form

```sql
CREATE TABLE parts AS
SELECT p.id,
       ST_3DTryFromWKB(p.geom, p.props) AS solid,
       b.b3_volume_lod22, b.b3_opp_grond, b.b3_h_maaiveld,
       len(b.children) AS n_parts
FROM feats p JOIN feats b ON b.id = p.parents[1]
WHERE p.geom IS NOT NULL;

SELECT count(*) AS parts,
       count(solid) AS imported,
       count(*) FILTER (WHERE ST_3DIsClosed(solid))   AS closed,
       count(*) FILTER (WHERE ST_3DIsManifold(solid)) AS manifold,
       count(*) FILTER (WHERE ST_3DIsOriented(solid)) AS oriented,
       count(*) FILTER (WHERE ST_3DValidationReport(solid).is_valid) AS valid
FROM parts;
```

```
┌───────┬──────────┬────────┬──────────┬──────────┬───────┐
│ parts │ imported │ closed │ manifold │ oriented │ valid │
├───────┼──────────┼────────┼──────────┼──────────┼───────┤
│ 1116  │ 1116     │ 1107   │ 1103     │ 1103     │ 1098  │
└───────┴──────────┴────────┴──────────┴──────────┴───────┘
```

**Every part imports, but 18 of 1116 (1.6 %) are not valid solids.** That is the normal
state of real reconstructed geometry, not a defect in the reader — `duckdb-3d` flags
without repairing. Measure only the valid subset.

## 9 — Why they fail

```sql
SELECT id, ST_3DValidationReport(solid) AS report
FROM parts WHERE NOT ST_3DValidationReport(solid).is_valid LIMIT 2;
```

```
    id = NL.IMBAG.Pand.0503100000031902-0
report = {'is_valid': false, 'is_closed': true, 'is_manifold': false, 'is_oriented': false,
          'solid_count': 1, 'shell_count': 1, 'face_count': 1316, 'open_edge_count': 0,
          'non_manifold_edge_count': 1, 'degenerate_face_count': 1, 'orientation_error_count': 2,
          'code': INVALID, 'message': 'Invalid solid: non-manifold edges, orientation inconsistent, degenerate faces'}

    id = NL.IMBAG.Pand.0503100000032799-0
report = {'is_valid': false, 'is_closed': false, 'is_manifold': true, 'is_oriented': true,
          'solid_count': 1, 'shell_count': 1, 'face_count': 115, 'open_edge_count': 9,
          'non_manifold_edge_count': 0, 'degenerate_face_count': 0, 'orientation_error_count': 0,
          'code': INVALID, 'message': 'Invalid solid: not closed'}
```

A 1316-face building undone by a *single* non-manifold edge is typical. The report gives
counts, not locations.

## 10 — The measurement family

```sql
CREATE TABLE good AS SELECT * FROM parts WHERE ST_3DValidationReport(solid).is_valid;

SELECT id,
       ROUND(ST_3DVolume(solid), 1)        AS volume_m3,
       ROUND(ST_3DSurfaceArea(solid), 1)   AS surface_m2,
       ROUND(ST_3DArea(solid), 1)          AS area_m2,
       ROUND(ST_3DFootprintArea(solid), 1) AS footprint_m2,
       ROUND(ST_3DPerimeter(solid), 1)     AS perimeter_m,
       ROUND(ST_3DZMin(solid), 2)          AS zmin,
       ROUND(ST_3DZMax(solid), 2)          AS zmax,
       ST_3DNumFaces(solid)                AS faces
FROM good ORDER BY id LIMIT 5;
```

```
┌──────────────────────────────────┬───────────┬────────────┬─────────┬──────────────┬─────────────┬───────┬───────┬───────┐
│                id                │ volume_m3 │ surface_m2 │ area_m2 │ footprint_m2 │ perimeter_m │ zmin  │ zmax  │ faces │
├──────────────────────────────────┼───────────┼────────────┼─────────┼──────────────┼─────────────┼───────┼───────┼───────┤
│ NL.IMBAG.Pand.0503100000000030-0 │ 137182.8  │ 39403.4    │ 39403.4 │ 15341.5      │ 0.0         │ 0.13  │ 17.53 │ 271   │
│ NL.IMBAG.Pand.0503100000000137-0 │ 796.8     │ 539.9      │ 539.9   │ 84.8         │ 0.0         │ -0.08 │ 9.64  │ 16    │
│ NL.IMBAG.Pand.0503100000000138-0 │ 20.7      │ 47.0       │ 47.0    │ 8.0          │ 0.0         │ 0.23  │ 2.84  │ 6     │
│ NL.IMBAG.Pand.0503100000000139-0 │ 398.1     │ 332.3      │ 332.3   │ 47.3         │ 0.0         │ 0.46  │ 8.96  │ 10    │
│ NL.IMBAG.Pand.0503100000000140-0 │ 18179.1   │ 5292.6     │ 5292.6  │ 1284.6       │ 0.0         │ 0.07  │ 17.95 │ 118   │
└──────────────────────────────────┴───────────┴────────────┴─────────┴──────────────┴─────────────┴───────┴───────┴───────┘
```

`ST_3DArea` is an alias of `ST_3DSurfaceArea` — identical by construction. **`ST_3DPerimeter`
is `0.0` on every row**, which is correct: it sums *boundary* edges (used by exactly one
face), and a closed solid has none. §11 shows it non-zero.

`ST_3DBounds` returns the cached box as a struct:

```sql
SELECT ST_3DBounds(solid) AS bounds FROM good ORDER BY id LIMIT 1;
```

```
bounds = {'min_x': 84501.553625, 'min_y': 446165.972, 'min_z': 0.12600262451172028,
          'max_x': 84729.745625, 'max_y': 446295.636, 'max_z': 17.530002624511717}
```

## 11 — `ST_3DPerimeter` on the nine unclosed parts

```sql
SELECT id,
       ST_3DValidationReport(solid).open_edge_count AS open_edges,
       ROUND(ST_3DPerimeter(solid), 3) AS perimeter_m
FROM parts WHERE NOT ST_3DIsClosed(solid) ORDER BY id LIMIT 4;
```

```
┌──────────────────────────────────┬────────────┬─────────────┐
│                id                │ open_edges │ perimeter_m │
├──────────────────────────────────┼────────────┼─────────────┤
│ NL.IMBAG.Pand.0503100000000010-0 │ 6          │ 23.245      │
│ NL.IMBAG.Pand.0503100000019817-0 │ 6          │ 23.172      │
│ NL.IMBAG.Pand.0503100000024960-0 │ 3          │ 38.297      │
│ NL.IMBAG.Pand.0503100000025026-0 │ 11         │ 88.948      │
└──────────────────────────────────┴────────────┴─────────────┘
```

Note `ST_3DPerimeter` has **no validity precondition** — it works on solids that
`ST_3DVolume` would reject, which is exactly when you want it.

## 12 — Distance and relationship family on `GEOM_3D`

All seven distance functions, plus a non-zero `ST_3DLength`.

```sql
CREATE TABLE g2 AS
SELECT (SELECT ST_Geom3DFromWKB(geom) FROM feats WHERE id = 'NL.IMBAG.Pand.0503100000012869-0') AS a,
       (SELECT ST_Geom3DFromWKB(geom) FROM feats WHERE id = 'NL.IMBAG.Pand.0503100000016459-0') AS b;

SELECT ROUND(ST_3DDistance(a, b), 3)    AS dist_m,
       ROUND(ST_3DMaxDistance(a, b), 3) AS maxdist_m,
       ST_3DDWithin(a, b, 1100.0)       AS dwithin_1100,
       ST_3DDWithin(a, b, 100.0)        AS dwithin_100,
       ST_3DDFullyWithin(a, b, 1200.0)  AS dfullywithin_1200,
       ST_3DIntersects(a, b)            AS intersects,
       ST_3DAsText(ST_3DClosestPoint(a, b)) AS closest_pt,
       ST_3DAsText(ST_3DShortestLine(a, b)) AS shortest_line,
       ROUND(ST_3DLength(ST_3DShortestLine(a, b)), 3) AS line_len
FROM g2;
```

```
dist_m            = 1033.745
maxdist_m         = 1041.808
dwithin_1100      = true
dwithin_100       = false
dfullywithin_1200 = true
intersects        = false
closest_pt        = POINT Z (85563.7526 446828.446 2.39100262)
shortest_line     = LINESTRING Z (85563.7526 446828.446 2.39100262, 84597.5076 446461.023 2.39100262)
line_len          = 1033.745
```

`ST_3DLength(ST_3DShortestLine(...))` reproduces `ST_3DDistance` exactly — a useful
self-consistency check. Self-comparison behaves too:

```sql
SELECT ST_3DIntersects(a, a) AS self_intersects, ROUND(ST_3DDistance(a, a), 6) AS self_dist FROM g2;
```

```
┌─────────────────┬───────────┐
│ self_intersects │ self_dist │
├─────────────────┼───────────┤
│ true            │ 0.0       │
└─────────────────┴───────────┘
```

## 13 — Cross-check against 3DBAG's own published figures

An independent oracle: 3DBAG ships each building's own computed volume and ground area.

```sql
SELECT count(*) AS n,
       ROUND(median(abs(ST_3DVolume(solid) - b3_volume_lod22) / b3_volume_lod22) * 100, 4)      AS median_vol_err_pct,
       ROUND(median(abs(ST_3DFootprintArea(solid) - b3_opp_grond) / b3_opp_grond) * 100, 4)     AS median_fp_err_pct
FROM good WHERE n_parts = 1 AND b3_volume_lod22 > 0 AND b3_opp_grond > 0;
```

```
┌──────┬────────────────────┬───────────────────┐
│  n   │ median_vol_err_pct │ median_fp_err_pct │
├──────┼────────────────────┼───────────────────┤
│ 1096 │ 0.0167             │ 0.0043            │
└──────┴────────────────────┴───────────────────┘
```

The automated version of this check is
[`test/sql/cityjson_delft_remote.test`](../test/sql/cityjson_delft_remote.test), which extends
it to volume at LoD1.2 and LoD1.3, surface area against the five `b3_opp_*` attributes summed,
`ST_3DZMin` against `b3_h_maaiveld` and the LoD1.2 extrusion height against `b3_h_dak_70p`.
[`test/sql/cityjson_3dbag_attributes.test`](../test/sql/cityjson_3dbag_attributes.test) runs
the same comparisons offline, over the frozen nine-building slice.

## 14 — Transforms: rigid motions and the cube law

```sql
CREATE TABLE ex AS
SELECT solid, ST_Geom3DFromWKB(ST_3DAsWKB(solid)) AS g
FROM good WHERE id = 'NL.IMBAG.Pand.0503100000012869-0';

SELECT ROUND(ST_3DVolume(solid), 4)                            AS v0,
       ROUND(ST_3DVolume(ST_3DTranslate(solid, 100, 50, 5)), 4) AS v_translate,
       ROUND(ST_3DVolume(ST_3DRotateX(solid, pi()/3)), 4)      AS v_rotx,
       ROUND(ST_3DVolume(ST_3DRotateY(solid, pi()/4)), 4)      AS v_roty,
       ROUND(ST_3DVolume(ST_3DRotateZ(solid, pi()/6)), 4)      AS v_rotz,
       ROUND(ST_3DVolume(ST_3DScale(solid, 2, 2, 2)) / ST_3DVolume(solid), 6) AS scale_ratio
FROM ex;
```

```
┌─────────┬─────────────┬─────────┬─────────┬─────────┬─────────────┐
│   v0    │ v_translate │ v_rotx  │ v_roty  │ v_rotz  │ scale_ratio │
├─────────┼─────────────┼─────────┼─────────┼─────────┼─────────────┤
│ 19.5254 │ 19.5254     │ 19.5254 │ 19.5254 │ 19.5254 │ 8.0         │
└─────────┴─────────────┴─────────┴─────────┴─────────┴─────────────┘
```

> **This cell used to fail.** `v_rotx` read `18.9956` — 2.7 % off — before the fix
> described in [Quirks](#a-real-bug-this-walkthrough-found-volume-drift-under-rotation). The output above is post-fix.

Across all 1098 valid parts, the relative volume drift under rotation is now zero to six
decimal places — the query below rounds, so read it as "below 5e-7", not as "exactly zero":

```sql
SELECT ROUND(max(abs(ST_3DVolume(ST_3DRotateX(solid, pi()/3)) - ST_3DVolume(solid))/ST_3DVolume(solid)), 6) AS max_relerr_rotx,
       ROUND(max(abs(ST_3DVolume(ST_3DRotateY(solid, pi()/3)) - ST_3DVolume(solid))/ST_3DVolume(solid)), 6) AS max_relerr_roty,
       ROUND(max(abs(ST_3DVolume(ST_3DRotateZ(solid, pi()/3)) - ST_3DVolume(solid))/ST_3DVolume(solid)), 6) AS max_relerr_rotz
FROM good;
```

```
┌─────────────────┬─────────────────┬─────────────────┐
│ max_relerr_rotx │ max_relerr_roty │ max_relerr_rotz │
├─────────────────┼─────────────────┼─────────────────┤
│ 0.0             │ 0.0             │ 0.0             │
└─────────────────┴─────────────────┴─────────────────┘
```

Unrounded, the same three maxima are **1.8e-11**, **5.0e-12** and **4.4e-11**, and the
translation maximum is *exactly* 0.0. Rotation is the looser case for a reason that has
nothing to do with the volume sum: rotating absolute RD coordinates injects `|p|·eps`
rounding into the vertices themselves before any measurement runs, so ~1e-11 is the floor
imposed by the coordinates, not a residue of the cancellation this section is about.

## 15 — `ST_3DTransform`: RD New → WGS84

```sql
SELECT ST_3DAsText(ST_3DCentroid(g)) AS rd_centroid,
       ST_3DAsText(ST_3DCentroid(ST_3DTransform(g, 28992, 4326)))                 AS wgs84_centroid,
       ST_3DAsText(ST_3DCentroid(ST_3DTransform(g, 'EPSG:28992', 'EPSG:4326')))   AS wgs84_str_form
FROM ex;
```

```
┌───────────────────────────────────────────┬───────────────────────────────────────────┬───────────────────────────────────────────┐
│                rd_centroid                │              wgs84_centroid               │              wgs84_str_form               │
├───────────────────────────────────────────┼───────────────────────────────────────────┼───────────────────────────────────────────┤
│ POINT Z (84595.382 446461.183 1.82866198) │ POINT Z (4.36190203 52.0020555 1.8422511) │ POINT Z (4.36190203 52.0020555 1.8422511) │
└───────────────────────────────────────────┴───────────────────────────────────────────┴───────────────────────────────────────────┘
```

Integer and string CRS forms agree. Output is **(lon, lat)** — axis order is normalised
to easting/northing, not EPSG:4326's declared (lat, lon). Z moved from `1.82866198` to
`1.8422511`, which looks like a vertical transformation but is not: reprojection is
horizontal-only, and the centroid is area-weighted, so a slightly different XY footprint
reweights it. See [FUTURE_WORK.md §2](./FUTURE_WORK.md) — vertical datum support is
deliberately deferred.

## 16 — Construction: hull, extrude, make-solid, force-3D

```sql
SELECT ST_3DGeometryType(ST_3DConvexHull(g))                        AS hull_type,
       ST_3DNumGeometries(ST_3DConvexHull(g))                       AS hull_parts,
       ROUND(ST_3DFootprintArea(ST_3DConvexHull(g)), 2)             AS hull_area,
       ST_3DGeometryType(ST_Force3D(g))                             AS force3d_type,
       ST_3DNumFaces(ST_3DExtrude(ST_3DConvexHull(g), 10.0))        AS prism_faces,
       ROUND(ST_3DVolume(ST_3DExtrude(ST_3DConvexHull(g), 10.0)), 2) AS prism_vol,
       ROUND(ST_3DFootprintArea(ST_3DConvexHull(g)) * 10.0, 2)      AS fp_x_h,
       ROUND(ST_3DVolume(ST_MakeSolid(ST_Geom3DFromWKB(ST_3DAsWKB(solid)))), 4) AS makesolid_vol
FROM ex;
```

```
┌────────────┬────────────┬───────────┬──────────────────────┬─────────────┬───────────┬────────┬───────────────┐
│ hull_type  │ hull_parts │ hull_area │     force3d_type     │ prism_faces │ prism_vol │ fp_x_h │ makesolid_vol │
├────────────┼────────────┼───────────┼──────────────────────┼─────────────┼───────────┼────────┼───────────────┤
│ ST_Polygon │ 1          │ 7.21      │ ST_PolyhedralSurface │ 6           │ 72.13     │ 72.13  │ 19.5254       │
└────────────┴────────────┴───────────┴──────────────────────┴─────────────┴───────────┴────────┴───────────────┘
```

`prism_vol` equals `hull_area × 10` exactly. `makesolid_vol` equals the original
`19.5254` from §14 — the `SOLID_3D → WKB → GEOM_3D → SOLID_3D` round trip is lossless.
`ST_Force3D` is currently an identity (`GEOM_3D` is already XYZ).

## 17 — Serialization

```sql
SELECT octet_length(ST_3DAsWKB(solid))  AS aswkb_bytes,
       octet_length(ST_3DAsBinary(g))   AS asbinary_bytes,
       length(ST_3DAsText(g))           AS wkt_chars,
       length(ST_3DAsGeoJSON(g))        AS geojson_chars,
       substr(ST_3DAsText(g), 1, 52)    AS wkt_head,
       substr(ST_3DAsGeoJSON(g), 1, 52) AS geojson_head
FROM ex;
```

```
┌─────────────┬────────────────┬───────────┬───────────────┬──────────────────────────────────────────────────────┬──────────────────────────────────────────────────────┐
│ aswkb_bytes │ asbinary_bytes │ wkt_chars │ geojson_chars │                       wkt_head                       │                     geojson_head                     │
├─────────────┼────────────────┼───────────┼───────────────┼──────────────────────────────────────────────────────┼──────────────────────────────────────────────────────┤
│ 807         │ 807            │ 1077      │ 1126          │ POLYHEDRALSURFACE Z (((84593.9846 446459.603 0.47500 │ {"type":"MultiPolygon","coordinates":[[[[84593.9846, │
└─────────────┴────────────────┴───────────┴───────────────┴──────────────────────────────────────────────────────┴──────────────────────────────────────────────────────┘
```

WKT says `POLYHEDRALSURFACE Z` but GeoJSON says `MultiPolygon` — GeoJSON has no
polyhedral-surface type, so it is emitted as a `MultiPolygon` (documented, not a bug).
`ST_3DAsWKB` (from `SOLID_3D`) and `ST_3DAsBinary` (from `GEOM_3D`) agree byte-for-byte:

```sql
SELECT ST_3DVolume(ST_MakeSolid(ST_Geom3DFromWKB(ST_3DAsWKB(solid)))) = ST_3DVolume(solid) AS volume_roundtrip,
       ST_3DAsBinary(ST_Geom3DFromWKB(ST_3DAsWKB(solid))) = ST_3DAsWKB(solid)              AS bytes_roundtrip
FROM ex;
```

```
┌──────────────────┬─────────────────┐
│ volume_roundtrip │ bytes_roundtrip │
├──────────────────┼─────────────────┤
│ true             │ true            │
└──────────────────┴─────────────────┘
```

## 18 — City scale: 77 000 Helsinki solids

A different CRS (EPSG:3879), a different producer, two orders of magnitude more objects.

```sql
CREATE TABLE hel AS
SELECT id, object_type, geometry_lod2_0 AS geom, geometry_properties_lod2_0 AS props
FROM read_cityjsonseq('https://cityjson.open3d.city/cityjsonseq/Helsinki_tex.city.jsonl',
                      lod => '2');

CREATE TABLE helsolids AS
SELECT id, ST_3DTryFromWKB(geom, props) AS s FROM hel WHERE geom IS NOT NULL;

SELECT count(*) AS objects, count(s) AS imported,
       count(*) FILTER (WHERE ST_3DIsClosed(s))   AS closed,
       count(*) FILTER (WHERE ST_3DIsManifold(s)) AS manifold,
       count(*) FILTER (WHERE ST_3DIsOriented(s)) AS oriented,
       count(*) FILTER (WHERE ST_3DValidationReport(s).is_valid) AS valid
FROM helsolids;
```

```
┌─────────┬──────────┬────────┬──────────┬──────────┬───────┐
│ objects │ imported │ closed │ manifold │ oriented │ valid │
├─────────┼──────────┼────────┼──────────┼──────────┼───────┤
│ 77249   │ 77249    │ 75541  │ 77055    │ 77046    │ 75474 │
└─────────┴──────────┴────────┴──────────┴──────────┴───────┘
```

97.7 % valid — the dominant failure mode here is *not closed* (1708 objects), unlike
Delft where non-manifoldness featured. Aggregate over the valid subset:

```sql
SELECT count(*) AS valid_objects,
       ROUND(SUM(ST_3DVolume(s))/1e9, 4)        AS total_volume_km3,
       ROUND(SUM(ST_3DFootprintArea(s))/1e6, 3) AS total_footprint_km2,
       ROUND(SUM(ST_3DVolume(s))/SUM(ST_3DFootprintArea(s)), 2) AS mean_height_m,
       ROUND(max(ST_3DZMax(s) - ST_3DZMin(s)), 2) AS tallest_m
FROM (SELECT s FROM helsolids WHERE ST_3DValidationReport(s).is_valid);
```

```
┌───────────────┬──────────────────┬─────────────────────┬───────────────┬───────────┐
│ valid_objects │ total_volume_km3 │ total_footprint_km2 │ mean_height_m │ tallest_m │
├───────────────┼──────────────────┼─────────────────────┼───────────────┼───────────┤
│ 75474         │ 0.1915           │ 16.065              │ 11.92         │ 119.5     │
└───────────────┴──────────────────┴─────────────────────┴───────────────┴───────────┘
```

An 11.9 m mean building height and a 119.5 m tallest structure are both plausible for
Helsinki — the sanity check that the numbers are physical, not just non-null.

**Note the subquery.** Writing `SUM(ST_3DVolume(s)) FILTER (WHERE ...is_valid)` does
*not* work: DuckDB evaluates the aggregate's argument for every row and only then
applies the filter, so `ST_3DVolume` raises on the first invalid solid. Filtering
functions with preconditions must happen in `WHERE`, not `FILTER`.

---

# Part C — CityParquet round trip

Nothing before this point proves the **stored** CityParquet encoding — a Parquet file with
`geometry_lod*` WKB columns and `geometry_properties_lod*` STRUCT columns — feeds back
into `SOLID_3D`. These cells do.

## 19 — Write a real CityParquet package

```sql
CREATE SCHEMA pkg;
CREATE TABLE pkg.building AS
SELECT * FROM read_cityjsonseq('../cityparquet-rs/tests/fixtures/delft.city.jsonl');

PRAGMA cityparquet_init('pkg');
SELECT table_name, role FROM pkg.__cityparquet ORDER BY 1;

PRAGMA cityparquet_validate('pkg');
SELECT * FROM cityparquet_validation;

SELECT * FROM cityparquet_write('pkg', '/tmp/cp_test/pkg_out', crs => 'EPSG:7415');
```

```
┌────────────┬────────┐        ┌──────────────────┬─────────┬──────┬─────────┐
│ table_name │  role  │        │       file       │ action  │ rows │  bytes  │
├────────────┼────────┤        ├──────────────────┼─────────┼──────┼─────────┤
│ building   │ object │        │ building.parquet │ written │ 2231 │ 3675602 │
└────────────┴────────┘        │ metadata.json    │ written │    0 │    6722 │
                               └──────────────────┴─────────┴──────┴─────────┘
```

`cityparquet_validation` is empty — no findings. The file carries four LoDs:

```sql
SELECT column_name FROM (DESCRIBE SELECT * FROM read_parquet('/tmp/cp_test/pkg_out/building.parquet'))
WHERE column_name LIKE 'geometry_lod%';
```

```
┌─────────────────┐
│   column_name   │
├─────────────────┤
│ geometry_lod0_0 │
│ geometry_lod1_2 │
│ geometry_lod1_3 │
│ geometry_lod2_2 │
└─────────────────┘
```

## 20 — Three sidecar forms, one solid

The stored STRUCT, the same STRUCT rendered to JSON text, and no sidecar at all. All
1116 solids import identically under all three.

```sql
SELECT geometry_properties_lod2_2.type   AS type,
       geometry_properties_lod2_2.shells AS shells
FROM read_parquet('/tmp/cp_test/pkg_out/building.parquet')
WHERE geometry_lod2_2 IS NOT NULL LIMIT 1;
```

```
  type = Solid
shells = [[6]]
```

```sql
LOAD json;
CREATE TABLE cp AS
SELECT id,
       ST_3DTryFromWKB(geometry_lod2_2, geometry_properties_lod2_2)                    AS s_struct,
       ST_3DTryFromWKB(geometry_lod2_2, to_json(geometry_properties_lod2_2)::VARCHAR)  AS s_json,
       ST_3DTryFromWKB(geometry_lod2_2)                                                AS s_bare
FROM read_parquet('/tmp/cp_test/pkg_out/building.parquet')
WHERE geometry_lod2_2 IS NOT NULL;

SELECT count(*) AS rows,
       count(s_struct) AS via_struct, count(s_json) AS via_json, count(s_bare) AS via_bare,
       count(*) FILTER (WHERE ST_3DAsWKB(s_struct) = ST_3DAsWKB(s_json)) AS struct_eq_json,
       count(*) FILTER (WHERE ST_3DAsWKB(s_struct) = ST_3DAsWKB(s_bare)) AS struct_eq_bare
FROM cp;
```

```
┌──────┬────────────┬──────────┬──────────┬────────────────┬────────────────┐
│ rows │ via_struct │ via_json │ via_bare │ struct_eq_json │ struct_eq_bare │
├──────┼────────────┼──────────┼──────────┼────────────────┼────────────────┤
│ 1116 │ 1116       │ 1116     │ 1116     │ 1116           │ 1116           │
└──────┴────────────┴──────────┴──────────┴────────────────┴────────────────┘
```

`struct_eq_bare = 1116` only because every solid in this tile is single-shell (`[[6]]`);
drop the sidecar on a hollow or multi-solid geometry and the partition is lost (§2, §3).

## 21 — Validation and measurement straight off Parquet

```sql
SELECT count(*) AS parts,
       count(*) FILTER (WHERE ST_3DIsClosed(s_struct))   AS closed,
       count(*) FILTER (WHERE ST_3DIsManifold(s_struct)) AS manifold,
       count(*) FILTER (WHERE ST_3DValidationReport(s_struct).is_valid) AS valid
FROM cp;

SELECT count(*) AS valid_parts,
       ROUND(SUM(ST_3DVolume(s_struct)), 0)        AS total_volume_m3,
       ROUND(SUM(ST_3DSurfaceArea(s_struct)), 0)   AS total_surface_m2,
       ROUND(SUM(ST_3DFootprintArea(s_struct)), 0) AS total_footprint_m2,
       ROUND(min(ST_3DZMin(s_struct)), 2)          AS zmin,
       ROUND(max(ST_3DZMax(s_struct)), 2)          AS zmax
FROM (SELECT s_struct FROM cp WHERE ST_3DValidationReport(s_struct).is_valid);
```

```
┌───────┬────────┬──────────┬───────┐
│ parts │ closed │ manifold │ valid │
├───────┼────────┼──────────┼───────┤
│ 1116  │ 1107   │ 1103     │ 1098  │
└───────┴────────┴──────────┴───────┘
┌─────────────┬─────────────────┬──────────────────┬────────────────────┬───────┬───────┐
│ valid_parts │ total_volume_m3 │ total_surface_m2 │ total_footprint_m2 │ zmin  │ zmax  │
├─────────────┼─────────────────┼──────────────────┼────────────────────┼───────┼───────┤
│ 1098        │ 1915861.0       │ 811292.0         │ 192844.0           │ -2.46 │ 40.04 │
└─────────────┴─────────────────┴──────────────────┴────────────────────┴───────┴───────┘
```

Identical to the CityJSONSeq counts in §8 — `1116 / 1107 / 1103 / 1098`. Proven directly:

```sql
SELECT count(*) AS matched,
       count(*) FILTER (WHERE ST_3DAsWKB(p.solid) = ST_3DAsWKB(c.s_struct)) AS identical_wkb,
       ROUND(max(abs(ST_3DFootprintArea(p.solid) - ST_3DFootprintArea(c.s_struct))), 12) AS max_fp_diff
FROM parts p JOIN cp c USING (id);
```

```
┌─────────┬───────────────┬─────────────┐
│ matched │ identical_wkb │ max_fp_diff │
├─────────┼───────────────┼─────────────┤
│ 1116    │ 1116          │ 0.0         │
└─────────┴───────────────┴─────────────┘
```

**Every one of 1116 solids is byte-identical** whether it arrives via streamed
CityJSONSeq or via a Parquet file on disk. That is the CityParquet round-trip guarantee.

## 22 — LoD0 is GeoParquet, and reads as GEOMETRY

CityParquet's LoD0 footprint column carries the **Parquet-native `GEOMETRY`** logical
type; the solid LoDs stay `BLOB`. A writer annotates a column exactly when it declares it
in the `geo` footer, and a `PolyhedralSurface Z` is not legal GeoParquet — but the sharper
reason is that DuckDB promotes any annotated column and converts it eagerly, and its
geometry model has no polyhedral surface. Annotating a solid column would therefore make
even `SELECT count(*)` over it fail, before any `ST_3D*` function saw a value. That
asymmetry is visible on read-back:

```sql
LOAD spatial;
SELECT column_name, column_type
FROM (DESCRIBE SELECT * FROM read_parquet('/tmp/cp_test/pkg_out/building.parquet'))
WHERE column_name LIKE 'geometry_lod%' ORDER BY 1;
```

```
┌─────────────────┬───────────────────────┐
│   column_name   │      column_type      │
├─────────────────┼───────────────────────┤
│ geometry_lod0_0 │ GEOMETRY('EPSG:7415') │
│ geometry_lod1_2 │ BLOB                  │
│ geometry_lod1_3 │ BLOB                  │
│ geometry_lod2_2 │ BLOB                  │
└─────────────────┴───────────────────────┘
```

**`EPSG:7415` is a rendering, not what is stored.** `cityjson`'s writer puts the whole
PROJJSON document in the logical type's `crs` parameter, and `spatial` — loaded here —
resolves it back to its authority code for display. Without `spatial` the same column
prints as `GEOMETRY('{"$schema":"https://proj.org/schemas/v0.5/projjson…')`. The Parquet
side is unambiguous:

```sql
SELECT name, logical_type IS NOT NULL AS annotated, length(logical_type) AS logical_type_len
FROM parquet_schema('/tmp/cp_test/pkg_out/building.parquet')
WHERE name LIKE 'geometry_lod%' ORDER BY 1;
```

```
┌─────────────────┬───────────┬──────────────────┐
│      name       │ annotated │ logical_type_len │
├─────────────────┼───────────┼──────────────────┤
│ geometry_lod0_0 │ true      │             2081 │
│ geometry_lod1_2 │ false     │             NULL │
│ geometry_lod1_3 │ false     │             NULL │
│ geometry_lod2_2 │ false     │             NULL │
└─────────────────┴───────────┴──────────────────┘
```

**`geometry_lod0_0::BLOB` does not work** — DuckDB v1.5.4 raises
`Unimplemented type for cast (GEOMETRY('EPSG:7415') -> BLOB) when casting from source
column geometry_lod0_0`. `SET enable_geoparquet_conversion=false` does not help either:
the promotion follows the logical type, not the `geo` footer, so the column is still
`GEOMETRY` with the setting off. The constructors take the column as it comes:

```sql
SELECT count(*) AS n,
       ROUND(max(abs(ST_3DFootprintArea(ST_Geom3DFromWKB(geometry_lod0_0))
                     - ST_Area(geometry_lod0_0))), 12) AS max_abs_diff
FROM read_parquet('/tmp/cp_test/pkg_out/building.parquet') WHERE geometry_lod0_0 IS NOT NULL;
```

```
┌──────┬───────────────┐
│  n   │ max_abs_diff  │
├──────┼───────────────┤
│ 1115 │ 4.7214047e-05 │
└──────┴───────────────┘
```

Routing the same column through `spatial`'s `ST_AsWKB` first is still valid and gives the
identical answer — it is simply no longer necessary:

```sql
SELECT ROUND(max(abs(ST_3DFootprintArea(ST_Geom3DFromWKB(geometry_lod0_0))
                     - ST_3DFootprintArea(ST_Geom3DFromWKB(ST_AsWKB(geometry_lod0_0))))), 12) AS direct_vs_bridged
FROM read_parquet('/tmp/cp_test/pkg_out/building.parquet') WHERE geometry_lod0_0 IS NOT NULL;
```

```
┌───────────────────┐
│ direct_vs_bridged │
├───────────────────┤
│               0.0 │
└───────────────────┘
```

This doubles as a **third independent oracle**: `ST_3DFootprintArea` agrees with
`spatial`'s GEOS-backed `ST_Area` to 4.7e-5 m² worst case across 1115 footprints — about
3e-9 relative on areas up to 15 000 m².

## 23 — All LoDs of one building, side by side

```sql
WITH raw AS (SELECT * FROM read_parquet('/tmp/cp_test/pkg_out/building.parquet'))
SELECT b.id AS building,
       ROUND(ST_3DFootprintArea(ST_Geom3DFromWKB(b.geometry_lod0_0)), 2) AS lod0_area,
       ROUND(ST_3DVolume(ST_3DFromWKB(p.geometry_lod1_2, p.geometry_properties_lod1_2)), 1) AS lod12_vol,
       ROUND(ST_3DVolume(ST_3DFromWKB(p.geometry_lod2_2, p.geometry_properties_lod2_2)), 1) AS lod22_vol,
       ROUND(ST_3DFootprintArea(ST_3DFromWKB(p.geometry_lod2_2, p.geometry_properties_lod2_2)), 2) AS lod22_fp
FROM raw p JOIN raw b ON b.id = p.parents[1]
WHERE p.geometry_lod2_2 IS NOT NULL AND b.geometry_lod0_0 IS NOT NULL
  AND ST_3DValidationReport(ST_3DFromWKB(p.geometry_lod1_2, p.geometry_properties_lod1_2)).is_valid
  AND ST_3DValidationReport(ST_3DFromWKB(p.geometry_lod2_2, p.geometry_properties_lod2_2)).is_valid
ORDER BY b.id LIMIT 5;
```

```
┌────────────────────────────────┬───────────┬───────────┬───────────┬──────────┐
│            building            │ lod0_area │ lod12_vol │ lod22_vol │ lod22_fp │
├────────────────────────────────┼───────────┼───────────┼───────────┼──────────┤
│ NL.IMBAG.Pand.0503100000000030 │ 15341.22  │ 127502.8  │ 137182.8  │ 15341.47 │
│ NL.IMBAG.Pand.0503100000000137 │ 84.82     │ 821.6     │ 796.8     │ 84.82    │
│ NL.IMBAG.Pand.0503100000000138 │ 8.01      │ 20.8      │ 20.7      │ 8.01     │
│ NL.IMBAG.Pand.0503100000000139 │ 55.68     │ 403.4     │ 398.1     │ 47.3     │
│ NL.IMBAG.Pand.0503100000000140 │ 1295.08   │ 19570.9   │ 18179.1   │ 1284.59  │
└────────────────────────────────┴───────────┴───────────┴───────────┴──────────┘
```

The **join is mandatory**: LoD0 lives on the `Building`, the solid LoDs on its
`BuildingPart`s, so no single row carries both. Without the join this query returns zero
rows — which is what happened on the first attempt and looked like a broken filter.

`lod0_area` tracks `lod22_fp` closely but not exactly (LoD0 is an independent
generalisation), and diverges most on `…139`, a multi-part building where only one part
is joined. LoD1.2 volume is a prism approximation and sits either side of LoD2.2.

## Quirks and known gaps

### A real bug this walkthrough found: volume drift under rotation

`ST_3DRotateX` / `ST_3DRotateY` appeared to break volume invariance on real data — up to
**12 % relative drift** across the Delft tile — while `ST_3DRotateZ` stayed clean. It was
not the transforms. `ComputeVolume` summed signed tetrahedra `a·(b×c)` about the
**absolute coordinate origin**, so the triple product scaled as |position|³ while the
answer scaled as |extent|³. In EPSG:28992 (easting ~8.5e4, northing ~4.5e5) that cancelled
roughly nine of the sixteen available digits. Only X and Y rotations mix the large
northing into Z, which is why Z rotation looked fine and hid the problem. A tetrahedron
translated to 1e8 reported `1.67e7` instead of `1/6`.

Fixed by referencing each tetrahedron to a point on the shell it belongs to — a no-op in
exact arithmetic. Drift across all 1098 valid Delft solids is now at most **4.4e-11**
relative under rotation and *exactly* 0 under translation (§14); the ~1e-11 is the floor
imposed by rotating absolute RD coordinates, not a residue of the cancellation.
Untransformed measurements did not change: agreement with 3DBAG's `b3_volume_lod22` is
still a 0.0167 % median error. Regression tests: `test/cpp/test_measurements.cpp`
("far-from-origin tetrahedron keeps full precision") and the RotateX/RotateY cases in
`test/sql/metamorphic_transforms.test`, where the translation tolerance also tightened
from 1e-3 to 1e-5 — that looseness had been a workaround for this very cancellation.

The first attempt at this fix used **one** reference point for the whole model (its
bounding-box midpoint). That is enough for a compact building but not in general: for a
`MultiSolid`/`CompositeSolid` whose parts are far apart the midpoint is far from *every*
part, and the same cancellation returns — two unit cubes separated by 1e6 reported 14.456
instead of 16, and by 1e7, 2353. The reference point has to be hoisted **per shell**; see
DESIGN_DOC §8.2 and `test/sql/st_3d_multisolid.test`. Two neighbouring conditioning bugs of
the same family turned up while pinning it — ear-clipping's handedness test (DESIGN_DOC §2.2)
and Newell's ring area, which made the degenerate-face verdict position-dependent
(DESIGN_DOC §8.1).

### Cells whose real output contradicts the naive expectation

- **§10 — `ST_3DPerimeter` is `0.0` on every valid building.** Correct: it sums edges used
  by exactly one face, and a closed solid has none. §11 shows it non-zero on open shells.
- **§6 — `ST_3DLength` is `0.0` on a `MultiPolygon`.** Not an error; it is defined for
  linear geometry only. §12 shows it non-zero on a `LineString`.
- **§4 — every LoD3 geometry in `lod3_railway.city.json` fails `ST_3DTryFromWKB`.** The
  file genuinely contains no solids, only `MultiSurface`/`CompositeSurface`. The strict
  form's message names WKB type code `1006` (`MultiPolygon Z`), not the CityJSON type,
  which makes it harder to diagnose than it should be.
- **§5 — footprint areas of ~1 m² for railway infrastructure.** The fixture's coordinates
  are not metric (extent ~12 units). Measurements are Cartesian in input units.
- **§8, §18 — 1.6 % of Delft and 2.3 % of Helsinki are invalid solids.** Expected for real
  reconstructions. `duckdb-3d` flags without repairing, by design.
- **§17 — `ST_3DAsText` says `POLYHEDRALSURFACE Z`, `ST_3DAsGeoJSON` says `MultiPolygon`.**
  GeoJSON has no polyhedral-surface type.
- **§15 — Z changes under `ST_3DTransform` despite horizontal-only reprojection.** The
  centroid is area-weighted, so a reprojected XY footprint reweights it. The Z
  *coordinates* pass through untouched.

### Traps that cost time

- **`FILTER` does not guard a raising function** (§18). `SUM(ST_3DVolume(s)) FILTER (WHERE
  …is_valid)` still evaluates `ST_3DVolume` on invalid rows and raises. Filter in `WHERE`.
- **`geometry_lod0_0::BLOB` raises** in DuckDB v1.5.4 (§22), and
  `SET enable_geoparquet_conversion=false` is not a way out — the promotion follows the
  Parquet logical type, not the `geo` footer. Pass the `GEOMETRY` column straight to
  `ST_Geom3DFromWKB`, which takes it.
- **LoD0 and the solid LoDs never share a row** in 3DBAG (§23). Join `parents`/`children`,
  or the query silently returns nothing.
- **The LoD suffix is normalised**: `lod => '2'` yields `geometry_lod2_0`.
- **The community `cityjson` build is not supported** — its flat `geometry` /
  `geometry_properties` columns do not bind against anything in this repo. See
  [Setup](#which-cityjson-build-this-notebook-uses--and-why-it-matters).

### Tracked gaps, not bugs

- **No stored SRID.** Both CRSs must be given on every `ST_3DTransform` call, and there is
  no cross-CRS mismatch detection. [FUTURE_WORK.md §2](./FUTURE_WORK.md).
- **No vertical datum / 3D reprojection.** Z passes through unchanged.
  [FUTURE_WORK.md §2](./FUTURE_WORK.md).
- **`proj.db` is not bundled** into the distributable `.duckdb_extension`, so §15 depends
  on a system PROJ install. [FUTURE_WORK.md §2](./FUTURE_WORK.md).
- **CityJSON vocabulary still leaks into the kernel** (`metadata_parser.cpp` knows
  `cityjsonType`, `shells`, `MultiSolid`). The neutral-grouping redesign is
  [FUTURE_WORK.md §1](./FUTURE_WORK.md).
- **`ST_3DConvexHull` is a 2D XY hull** at minimum Z, not a true 3D hull — that needs the
  deferred CGAL/SFCGAL backend ([DESIGN_DOC.md §16](./DESIGN_DOC.md)).

---

## Running it

This notebook is **not** wired into an automated target — it needs the sibling
`duckdb-cityjson` build, network access, and several minutes of Helsinki parsing. Run it
by hand with the [Setup](#setup) incantation.

The behaviours it demonstrates are covered automatically as follows:

| Cells | Automated coverage |
| --- | --- |
| §1 | `test/sql/cityjson_interop.test` (gated on `require cityjson`) |
| §2 | `test/sql/cityjson_hollow_solid.test`, `test/sql/st_3d_hollow_solid.test`, `test/cpp/test_inner_shell.cpp` |
| §3 | `test/sql/cityjson_multisolid.test`, `test/sql/st_3d_multisolid.test` |
| §5, §6, §17 | `test/sql/geom_3d_accessors.test`, `geom_3d_measurements.test`, `geom_3d_serialize.test` |
| §8–§13 | `test/sql/cityjson_delft_remote.test` and `test/sql/cityjson_3dbag_attributes.test` (the 3DBAG oracle, remote and offline), `test/sql/st_3d_validation.test`, `test/sql/geom_3d_distance.test` |
| §14 | `test/sql/metamorphic_transforms.test` — **extended by this walkthrough** with the RotateX/RotateY cases from the quirks section |
| §14 (kernel) | `test/cpp/test_measurements.cpp` — **added by this walkthrough**: far-from-origin precision |
| §15 | `test/sql/st_transform.test`, `test/cpp/test_crs_transform.cpp` |
| §16 | `test/sql/geom_3d_construct.test`, `test/cpp/test_geom_construct.cpp` |
| §20 | `test/sql/st_3d_from_wkb_struct.test` (the CityParquet STRUCT sidecar) |
| §22 (GEOMETRY input) | `test/sql/wkb_from_geometry.test` (the constructors' `GEOMETRY` argument) |
| §22 (oracle) | Partly `test/sql/postgis_oracle.test`; the `spatial` cross-check here is manual |

**Genuinely uncovered by any automated test:** the CityParquet write→read round trip
(§19–§23). It is a candidate for a new gated test file; see [TEST_COVERAGE.md](./TEST_COVERAGE.md) for the oracle strategy.

Remember `THREE_D_TEST_FIXTURES=1` if you run `build/release/duckdb` directly — without
it the `st_aswkb*` helpers are unregistered and every file declaring
`require-env THREE_D_TEST_FIXTURES` silently skips.
