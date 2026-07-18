-- postgis_oracle_pairs.sql
-- Phase A, pair queries for the duckdb-3d differential harness (design doc §9.5.1).
--
-- Contract: the caller (gen_golden.py) has created and populated
--
--     pair_inputs(feature_a text, feature_b text,
--                 wkb_a text, wkb_b text, threshold double precision)
--
-- where wkb_a/wkb_b are the frozen ISO WKB of the two geometries. Emits one
-- reference CSV row per pair on stdout (COPY TO STDOUT).
--
-- Unlike ST_Volume/ST_3DArea, SFCGAL's 3D distance and relation predicates do
-- NOT require planar faces — they compute on the real non-planar 3DBAG surfaces
-- directly, so no exception wrapping is needed here (a raise should fail Phase A
-- loudly rather than be masked).

CREATE EXTENSION IF NOT EXISTS postgis;
CREATE EXTENSION IF NOT EXISTS postgis_sfcgal;
SET client_min_messages TO warning;

COPY (
  SELECT
    p.feature_a,
    p.feature_b,
    p.threshold,
    ST_3DDistance(ga.g, gb.g)                    AS pg_dist3d,
    ST_3DMaxDistance(ga.g, gb.g)                 AS pg_maxdist3d,
    ST_3DIntersects(ga.g, gb.g)                  AS pg_intersects,
    ST_3DDWithin(ga.g, gb.g, p.threshold)        AS pg_dwithin,
    ST_3DDFullyWithin(ga.g, gb.g, p.threshold)   AS pg_dfullywithin,
    -- Scalars from the geometry-returning functions (never round-trip PostGIS
    -- geometry back — it emits EWKB the extension's ISO parser rejects). The
    -- shortest line's length and the closest point's distance to gb both equal
    -- the 3D distance by construction, so they double as a consistency check.
    ST_3DLength(ST_3DShortestLine(ga.g, gb.g))          AS pg_shortline_len,
    ST_3DDistance(ST_3DClosestPoint(ga.g, gb.g), gb.g)  AS pg_closestpoint_dist
  FROM pair_inputs p
  CROSS JOIN LATERAL (SELECT ST_GeomFromWKB(decode(p.wkb_a, 'hex')) AS g) ga
  CROSS JOIN LATERAL (SELECT ST_GeomFromWKB(decode(p.wkb_b, 'hex')) AS g) gb
  ORDER BY p.feature_a, p.feature_b, p.threshold
) TO STDOUT WITH (FORMAT CSV, HEADER true);
