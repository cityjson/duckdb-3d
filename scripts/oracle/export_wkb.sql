-- export_wkb.sql
-- Phase A, DuckDB side: emit the (feature_id, lod, geom_role, wkb_hex) inputs
-- for the PostGIS differential oracle as CSV on stdout.
--
-- Run through the release CLI (build/release/duckdb, which has three_d
-- preloaded) with cityjson loaded. gen_golden.py loads the LOCAL duckdb-cityjson
-- build for us (`-cmd "LOAD '<path>'"`, see CITYJSON_EXTENSION there) — this
-- script no longer installs the community extension, whose column shape is
-- stale (see docs/CITYJSON_INTEROP.md).
--
-- wkb_hex is the extension's own ISO WKB export (ST_3DAsWKB for SOLID_3D,
-- ST_3DAsBinary for GEOM_3D); Phase A feeds these exact bytes to PostGIS and
-- Phase B re-imports them, so both engines see identical geometry.
--
-- geom_role partitions the input set by which side of the API consumes it:
--   'solid' — imports through ST_3DFromWKB into SOLID_3D (volume, shells, …)
--   'geom'  — imports through ST_Geom3DFromWKB into GEOM_3D (accessors,
--             serialization, planarity — classes a SOLID_3D cannot hold)
--
-- Analytic fixtures come first, then every geometry-bearing 3DBAG solid at
-- LoD 2.2.
--
-- The ST_AsWKB* fixture generators are registered only when THREE_D_TEST_FIXTURES
-- is set in the environment (see src/functions/fixtures.cpp); gen_golden.py sets
-- it for us, so run this script through gen_golden.py --reexport rather than by
-- hand, or export the variable first.

COPY (
  -- ── SOLID_3D fixtures ──────────────────────────────────────────────────
  SELECT 'fixture:tetra' AS feature_id, '' AS lod, 'solid' AS geom_role,
         lower(hex(ST_3DAsWKB(ST_3DFromWKB(ST_AsWKBPolyhedralTetra())))) AS wkb_hex
  UNION ALL
  SELECT 'fixture:open_tetra', '', 'solid',
         lower(hex(ST_3DAsWKB(ST_3DFromWKB(ST_AsWKBOpenTetra()))))
  UNION ALL
  -- Interior shell (V = 64 - 8 = 56) and a two-member collection (V = 8 + 8).
  -- SFCGAL rejects the hollow cube outright ('not connected'), which is itself
  -- worth freezing: it is the documented case where this extension, not PostGIS,
  -- is the oracle of record.
  SELECT 'fixture:hollow_cube', '', 'solid',
         lower(hex(ST_3DAsWKB(ST_3DFromWKB(ST_AsWKBHollowCube()))))
  UNION ALL
  SELECT 'fixture:multi_cube', '', 'solid',
         lower(hex(ST_3DAsWKB(ST_3DFromWKB(ST_AsWKBMultiCube()))))

  -- ── GEOM_3D fixtures: one per class the accessor surface dispatches on ──
  UNION ALL
  SELECT 'fixture:point', '', 'geom',
         lower(hex(ST_3DAsBinary(ST_Geom3DFromWKB(ST_AsWKBPointZ(1.5, 2.5, 3.5)))))
  UNION ALL
  SELECT 'fixture:line', '', 'geom',
         lower(hex(ST_3DAsBinary(ST_Geom3DFromWKB(ST_AsWKBLineZ()))))
  UNION ALL
  SELECT 'fixture:multiline', '', 'geom',
         lower(hex(ST_3DAsBinary(ST_Geom3DFromWKB(ST_AsWKBMultiLineZ()))))
  UNION ALL
  SELECT 'fixture:polygon', '', 'geom',
         lower(hex(ST_3DAsBinary(ST_Geom3DFromWKB(ST_AsWKBPolygonZ()))))
  UNION ALL
  -- Deliberately non-planar, so SFCGAL's ST_IsPlanar has a false to report.
  SELECT 'fixture:warped_polygon', '', 'geom',
         lower(hex(ST_3DAsBinary(ST_Geom3DFromWKB(ST_AsWKBWarpedPolygonZ()))))
  UNION ALL
  SELECT 'fixture:multipoint', '', 'geom',
         lower(hex(ST_3DAsBinary(ST_Geom3DFromWKB(ST_AsWKBMultiPointZ()))))
  UNION ALL
  SELECT 'fixture:multipolygon', '', 'geom',
         lower(hex(ST_3DAsBinary(ST_Geom3DFromWKB(ST_AsWKBMultiPolygonZ()))))

  -- ── Real 3DBAG geometry ────────────────────────────────────────────────
  UNION ALL
  SELECT id, '2.2', 'solid',
         lower(hex(ST_3DAsWKB(ST_3DTryFromWKB(geometry_lod2_2, geometry_properties_lod2_2))))
  FROM read_cityjsonseq('test/data/3dbag.city.jsonl', lod => '2.2')
  WHERE geometry_lod2_2 IS NOT NULL
) TO '/dev/stdout' WITH (FORMAT CSV, HEADER true);
