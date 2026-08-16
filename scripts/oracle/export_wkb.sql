-- export_wkb.sql
-- Phase A, DuckDB side: emit the (feature_id, lod, geom_role, wkb_hex) inputs
-- for the PostGIS differential oracle as CSV on stdout.
--
-- Run through the release CLI (build/release/duckdb, which has three_d
-- preloaded) with cityjson loaded. wkb_hex is the extension's own ISO WKB
-- export (ST_3DAsWKB); Phase A feeds these exact bytes to PostGIS and Phase B
-- re-imports them with ST_3DFromWKB, so both engines see identical geometry.
--
-- Analytic fixtures come first (tetra = valid, open_tetra = invalid open shell),
-- then every geometry-bearing 3DBAG solid at LoD 2.2.
--
-- The ST_AsWKB* fixture generators are registered only when THREE_D_TEST_FIXTURES
-- is set in the environment (see src/functions/fixtures.cpp); gen_golden.py sets
-- it for us, so run this script through gen_golden.py --reexport rather than by
-- hand, or export the variable first.

INSTALL cityjson FROM community;
LOAD cityjson;

COPY (
  SELECT 'fixture:tetra' AS feature_id, '' AS lod, 'solid' AS geom_role,
         lower(hex(ST_3DAsWKB(ST_3DFromWKB(ST_AsWKBPolyhedralTetra())))) AS wkb_hex
  UNION ALL
  SELECT 'fixture:open_tetra', '', 'solid',
         lower(hex(ST_3DAsWKB(ST_3DFromWKB(ST_AsWKBOpenTetra()))))
  UNION ALL
  SELECT id, '2.2', 'solid',
         lower(hex(ST_3DAsWKB(ST_3DTryFromWKB(geometry, geometry_properties))))
  FROM read_cityjsonseq('test/data/3dbag.city.jsonl', lod => '2.2')
  WHERE geometry IS NOT NULL
) TO '/dev/stdout' WITH (FORMAT CSV, HEADER true);
