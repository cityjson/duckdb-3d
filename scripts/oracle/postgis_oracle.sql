-- postgis_oracle.sql
-- Reference oracle queries for the duckdb-3d differential test harness (Phase A).
--
-- Contract: the caller (scripts/oracle/gen_golden.py) has already created and
-- populated a table
--
--     wkb_inputs(feature_id text, lod text, geom_role text, wkb_hex text)
--
-- where wkb_hex is ISO WKB (PolyhedralSurface Z) exported from the three_d
-- extension via ST_3DAsWKB. This script feeds the SAME bytes into PostGIS +
-- SFCGAL and emits one reference CSV row per input on stdout (COPY TO STDOUT).
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
    v.status                AS pg_volume_status
  FROM wkb_inputs i
  CROSS JOIN LATERAL (SELECT ST_GeomFromWKB(decode(i.wkb_hex, 'hex')) AS geom) g
  CROSS JOIN LATERAL oracle_area3d(g.geom) a
  CROSS JOIN LATERAL oracle_volume(g.geom) v
  ORDER BY i.geom_role, i.feature_id, i.lod
) TO STDOUT WITH (FORMAT CSV, HEADER true);
