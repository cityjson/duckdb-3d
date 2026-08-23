-- postgis_oracle_transforms.sql
-- Phase A, affine/CRS transform queries for the duckdb-3d differential harness.
--
-- Contract: the caller (gen_golden.py) has created and populated the same
--
--     wkb_inputs(feature_id text, lod text, geom_role text, wkb_hex text)
--
-- table postgis_oracle.sql consumes. Emits one reference CSV row per
-- (geometry, op) on stdout (COPY TO STDOUT).
--
-- Runs AFTER postgis_oracle.sql in the same database: the oracle_area3d /
-- oracle_volume wrappers it calls are created there (they persist beyond the
-- psql session; only wkb_inputs, being TEMP, is reloaded per invocation).
--
-- Why a separate file/CSV: each op needs a whole 3D bounding box plus the SFCGAL
-- measurements, so folding them into postgis_oracle.sql would mean six near-
-- identical column blocks. A long-form (feature_id, op, …) table stays readable
-- and lets ops be added without reshaping the per-geometry golden.
--
-- The op parameters are arbitrary but FIXED — changing one churns every value.
-- They are chosen to be discriminating: a translation with a distinct value per
-- axis, an anisotropic scale (whose det = 2·3·0.5 = 3 > 0, so orientation and
-- therefore SOLID_3D validity survive), and a rotation angle that is neither a
-- right angle nor symmetric under axis swaps.
--
--   translate  ST_Translate(g, 100.5, -50.25, 5.125)
--   scale      ST_Scale(g, ST_MakePoint(2, 3, 0.5))
--   rotatex/y/z ST_RotateX/Y/Z(g, 0.7 rad)
--   transform_28992_4326  ST_Transform(ST_SetSRID(g, 28992), 4326)
--
-- PostGIS's ST_RotateX/Y/Z are right-handed and counter-clockwise in radians,
-- which is the convention three_d's ST_3DRotateX/Y/Z document. ST_Transform is
-- horizontal-only for XYZ input (Z passes through), which is the convention
-- three_d's ST_3DTransform documents.
--
-- The reprojection op is emitted for the 3DBAG rows only: the fixtures sit at
-- the origin, far outside EPSG:28992's area of use, where PROJ's fallback
-- pipeline choice is not stable across PROJ versions.
--
-- Volume/area are re-measured after each op — that is the point. A rotation must
-- not change either (the bug fixed in b019978 made volume rotation-dependent on
-- real-world coordinates), and the anisotropic scale must multiply volume by
-- exactly det = 3. SFCGAL still rejects non-planar faces, so real 3DBAG rows
-- mostly record 'rejected' and the analytic fixtures carry these checks.

CREATE EXTENSION IF NOT EXISTS postgis;
CREATE EXTENSION IF NOT EXISTS postgis_sfcgal;
SET client_min_messages TO warning;

COPY (
  WITH src AS (
    SELECT i.feature_id, i.geom_role,
           ST_GeomFromWKB(decode(i.wkb_hex, 'hex')) AS geom
    FROM wkb_inputs i
  ),
  ops AS (
    SELECT feature_id, geom_role, 'translate' AS op,
           ST_Translate(geom, 100.5, -50.25, 5.125) AS geom FROM src
    UNION ALL
    SELECT feature_id, geom_role, 'scale',
           ST_Scale(geom, ST_MakePoint(2.0, 3.0, 0.5)) FROM src
    UNION ALL
    SELECT feature_id, geom_role, 'rotatex', ST_RotateX(geom, 0.7) FROM src
    UNION ALL
    SELECT feature_id, geom_role, 'rotatey', ST_RotateY(geom, 0.7) FROM src
    UNION ALL
    SELECT feature_id, geom_role, 'rotatez', ST_RotateZ(geom, 0.7) FROM src
    UNION ALL
    SELECT feature_id, geom_role, 'transform_28992_4326',
           ST_Transform(ST_SetSRID(geom, 28992), 4326) FROM src
    WHERE feature_id NOT LIKE 'fixture:%'
  )
  SELECT
    o.feature_id,
    o.geom_role,
    o.op,
    ST_XMin(o.geom) AS pg_min_x, ST_YMin(o.geom) AS pg_min_y, ST_ZMin(o.geom) AS pg_min_z,
    ST_XMax(o.geom) AS pg_max_x, ST_YMax(o.geom) AS pg_max_y, ST_ZMax(o.geom) AS pg_max_z,
    a.area   AS pg_area3d,
    a.status AS pg_area_status,
    v.vol    AS pg_volume,
    v.status AS pg_volume_status,
    -- Needs no planarity, so unlike area/volume these carry the 'geom' rows
    -- (whose classes have no SOLID_3D measurement to compare) through every op.
    ST_3DLength(o.geom)         AS pg_length3d,
    ST_Area(ST_Force2D(o.geom)) AS pg_proj_area
  FROM ops o
  CROSS JOIN LATERAL oracle_area3d(o.geom) a
  CROSS JOIN LATERAL oracle_volume(o.geom) v
  ORDER BY o.feature_id, o.op
) TO STDOUT WITH (FORMAT CSV, HEADER true);
