-- postgis_oracle.sql
-- Reference oracle queries for the duckdb-3d differential test harness (Phase A).
--
-- Contract: the caller (scripts/oracle/gen_golden.py) has already created and
-- populated a table
--
--     wkb_inputs(feature_id text, lod text, geom_role text, wkb_hex text)
--
-- where wkb_hex is ISO WKB exported from the three_d extension (ST_3DAsWKB for
-- the 'solid' rows, ST_3DAsBinary for the 'geom' rows). This script feeds the
-- SAME bytes into PostGIS + SFCGAL and emits one reference CSV row per input on
-- stdout (COPY TO STDOUT).
--
-- PostGIS/SFCGAL is the reference oracle only; it is never a build, runtime, or
-- CI dependency of the extension. See docs/DESIGN_DOC.md §9.5.
--
-- IMPORTANT — SFCGAL requires exactly-planar polygon faces. Real 3DBAG roof
-- surfaces are not perfectly coplanar after quantisation, so SFCGAL *rejects*
-- them ("points don't lie in the same plane") rather than approximating. Every
-- SFCGAL call is therefore wrapped so one bad face records status = 'rejected'
-- for that metric instead of aborting the batch; the extension (which measures
-- such faces by triangulation) is cross-checked against 3DBAG attributes there.
--
-- The same wrapping covers the class-restricted PostGIS accessors: ST_X/Y/Z
-- raise on anything but a Point, ST_IsPlanar on anything but a Polygon, and
-- ST_AsGeoJSON on a PolyhedralSurface. Those record NULL + status 'rejected'
-- rather than needing a per-class query.

CREATE EXTENSION IF NOT EXISTS postgis;
CREATE EXTENSION IF NOT EXISTS postgis_sfcgal;

-- SFCGAL emits verbose NOTICE dumps of the offending geometry; keep stdout clean.
SET client_min_messages TO warning;

-- Enclosed volume. ST_MakeSolid does no validation and ST_Volume returns a
-- negative value for an inward-oriented shell, so abs() folds the orientation
-- sign (a magnitude match, not a bug). Raises -> status 'rejected'.
CREATE OR REPLACE FUNCTION oracle_volume(g geometry,
                                         OUT vol double precision,
                                         OUT status text)
AS $$
BEGIN
  vol := abs(ST_Volume(ST_MakeSolid(g)));
  status := 'ok';
EXCEPTION WHEN others THEN
  vol := NULL;
  status := 'rejected';
END;
$$ LANGUAGE plpgsql;

-- Total surface area, taken on the un-solidified PolyhedralSurface (SFCGAL
-- returns 0 for the surface area of a solid-flagged geometry). Raises on
-- non-planar faces -> status 'rejected'.
CREATE OR REPLACE FUNCTION oracle_area3d(g geometry,
                                         OUT area double precision,
                                         OUT status text)
AS $$
BEGIN
  area := ST_3DArea(g);
  status := 'ok';
EXCEPTION WHEN others THEN
  area := NULL;
  status := 'rejected';
END;
$$ LANGUAGE plpgsql;

-- SFCGAL's planarity predicate. Defined for polygons only ("is_planar() only
-- applies to polygons"); every other class records 'rejected'.
CREATE OR REPLACE FUNCTION oracle_isplanar(g geometry,
                                           OUT planar boolean,
                                           OUT status text)
AS $$
BEGIN
  planar := ST_IsPlanar(g);
  status := 'ok';
EXCEPTION WHEN others THEN
  planar := NULL;
  status := 'rejected';
END;
$$ LANGUAGE plpgsql;

-- Point ordinates. ST_X/Y/Z raise on any non-Point, matching three_d's
-- ST_3DX/Y/Z, which raise on the same inputs.
CREATE OR REPLACE FUNCTION oracle_xyz(g geometry,
                                      OUT x double precision,
                                      OUT y double precision,
                                      OUT z double precision,
                                      OUT status text)
AS $$
BEGIN
  x := ST_X(g); y := ST_Y(g); z := ST_Z(g);
  status := 'ok';
EXCEPTION WHEN others THEN
  x := NULL; y := NULL; z := NULL;
  status := 'rejected';
END;
$$ LANGUAGE plpgsql;

-- GeoJSON. PostGIS has no GeoJSON encoding for PolyhedralSurface and raises
-- ("'PolyhedralSurface' geometry type not supported"), so those rows record
-- 'rejected'; three_d emits a MultiPolygon there instead (documented divergence).
CREATE OR REPLACE FUNCTION oracle_geojson(g geometry,
                                          OUT gj text,
                                          OUT status text)
AS $$
BEGIN
  gj := ST_AsGeoJSON(g);
  status := 'ok';
EXCEPTION WHEN others THEN
  gj := NULL;
  status := 'rejected';
END;
$$ LANGUAGE plpgsql;

COPY (
  SELECT
    i.feature_id,
    i.lod,
    i.geom_role,
    i.wkb_hex,
    ST_GeometryType(g.geom) AS pg_geomtype,
    ST_IsClosed(g.geom)     AS pg_is_closed,
    a.area                  AS pg_area3d,
    a.status                AS pg_area_status,
    v.vol                   AS pg_volume,
    v.status                AS pg_volume_status,
    -- 2D-XY convex-hull area. GEOS's ST_ConvexHull rejects a PolyhedralSurface,
    -- so feed it the vertex set via ST_Points (matches the extension, which hulls
    -- all XY-projected vertices). No planarity requirement, so no wrapper needed.
    ST_Area(ST_ConvexHull(ST_Points(g.geom))) AS pg_hull_area,

    -- ── Accessors (GEOS/liblwgeom; no planarity requirement, every row) ──
    ST_NDims(g.geom)        AS pg_ndims,
    ST_CoordDim(g.geom)     AS pg_coorddim,
    ST_Dimension(g.geom)    AS pg_dimension,
    ST_NumGeometries(g.geom) AS pg_numgeom,
    ST_3DLength(g.geom)     AS pg_length3d,
    -- Sum of the |XY projections| of every patch. For a two-sided closed shell
    -- each vertical column crosses it twice, so three_d's ST_3DFootprintArea is
    -- HALF of this; for (multi)polygons the two agree directly. The test applies
    -- the halving by class rather than baking it in here.
    ST_Area(ST_Force2D(g.geom)) AS pg_proj_area,

    -- ── 3D bounding box (exact on every row, including SFCGAL-rejected ones) ──
    ST_XMin(g.geom) AS pg_min_x, ST_YMin(g.geom) AS pg_min_y, ST_ZMin(g.geom) AS pg_min_z,
    ST_XMax(g.geom) AS pg_max_x, ST_YMax(g.geom) AS pg_max_y, ST_ZMax(g.geom) AS pg_max_z,

    -- ── Class-restricted accessors (wrapped; see the functions above) ──
    p.planar   AS pg_is_planar,
    p.status   AS pg_is_planar_status,
    xyz.x      AS pg_x,
    xyz.y      AS pg_y,
    xyz.z      AS pg_z,
    xyz.status AS pg_xyz_status,

    -- ── Serialization ──
    -- Does PostGIS re-emit the input bytes unchanged? If it does (it does, for
    -- every fixture and 3DBAG row), then asserting three_d's ST_3DAsBinary /
    -- ST_3DAsWKB against wkb_hex is a genuine two-implementation agreement on
    -- the ISO WKB encoding, not a self-comparison.
    (encode(ST_AsBinary(g.geom), 'hex') = i.wkb_hex) AS pg_wkb_roundtrip,
    ST_AsText(g.geom) AS pg_wkt,
    gj.gj             AS pg_geojson,
    gj.status         AS pg_geojson_status
  FROM wkb_inputs i
  CROSS JOIN LATERAL (SELECT ST_GeomFromWKB(decode(i.wkb_hex, 'hex')) AS geom) g
  CROSS JOIN LATERAL oracle_area3d(g.geom) a
  CROSS JOIN LATERAL oracle_volume(g.geom) v
  CROSS JOIN LATERAL oracle_isplanar(g.geom) p
  CROSS JOIN LATERAL oracle_xyz(g.geom) xyz
  CROSS JOIN LATERAL oracle_geojson(g.geom) gj
  ORDER BY i.geom_role, i.feature_id, i.lod
) TO STDOUT WITH (FORMAT CSV, HEADER true);
