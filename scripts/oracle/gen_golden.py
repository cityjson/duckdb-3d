#!/usr/bin/env python3
"""gen_golden.py — Phase A of the duckdb-3d PostGIS/SFCGAL differential harness.

Generates three frozen CSVs under test/data/postgis_oracle/ — reference values
computed by PostGIS + SFCGAL on the *same* ISO WKB bytes the three_d extension
exports:

  * golden.csv            per-geometry measurement, accessors, bbox, serialization
  * golden_pairs.csv      per-pair distance and relation predicates
  * golden_transforms.csv per (geometry, affine/CRS op) bbox and re-measurement

This is the ONLY place PostGIS exists — it runs offline, dev-time only. The
committed golden.csv is what the CI test (test/sql/postgis_oracle.test) compares
against, so `make test` never needs PostGIS, a container, or the network.

The WKB inputs are frozen in golden.csv, so there are two regen modes:

  * default (`just oracle-regen`) — read the frozen wkb_hex straight from the
    existing golden.csv and recompute the PostGIS/SFCGAL reference values. Needs
    ONLY the oracle container; no DuckDB, no cityjson, no fixture. Use this to
    refresh values when the PostGIS/SFCGAL version changes.

  * `--reexport` (`just oracle-reexport`) — re-derive the wkb_hex from the
    fixture via the release DuckDB CLI + cityjson, then recompute. Use this only
    when the fixture (test/data/3dbag.city.jsonl) or the input set changes. It
    needs a LOCAL duckdb-cityjson build (CITYJSON_EXTENSION, defaulting to the
    sibling checkout); the community build is no longer supported — its column
    shape is stale. See docs/CITYJSON_INTEROP.md.

Both modes then feed the bytes into PostGIS (scripts/oracle/postgis_oracle.sql,
_pairs.sql, _transforms.sql), sort rows, and fixed-format floats -> deterministic
CSVs (+ provenance). The oracle runs in a container managed by `just oracle-up`
(Apple `container`, Docker- and Podman-compatible via ORACLE_RUNTIME); a clean
run leaves the CSVs byte-identical.
"""

from __future__ import annotations

import argparse
import csv
import io
import math
import os
import re
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
DUCKDB = REPO / "build" / "release" / "duckdb"
EXPORT_SQL = REPO / "scripts" / "oracle" / "export_wkb.sql"
ORACLE_SQL = REPO / "scripts" / "oracle" / "postgis_oracle.sql"
ORACLE_PAIRS_SQL = REPO / "scripts" / "oracle" / "postgis_oracle_pairs.sql"
ORACLE_TRANSFORMS_SQL = REPO / "scripts" / "oracle" / "postgis_oracle_transforms.sql"
OUT_CSV = REPO / "test" / "data" / "postgis_oracle" / "golden.csv"
OUT_PAIRS_CSV = REPO / "test" / "data" / "postgis_oracle" / "golden_pairs.csv"
OUT_TRANSFORMS_CSV = REPO / "test" / "data" / "postgis_oracle" / "golden_transforms.csv"

# The cityjson reader used by --reexport. A LOCAL duckdb-cityjson build, loaded
# by path: the published community extension still emits the pre-per-LoD column
# shape (flat `geometry` BLOB) this repo no longer targets.
CITYJSON_EXTENSION = os.environ.get(
    "CITYJSON_EXTENSION",
    str(REPO.parent / "duckdb-cityjson" / "build" / "release" / "extension"
        / "cityjson" / "cityjson.duckdb_extension"))

# Container runtime + image are configurable so the harness also runs under
# plain Docker; the defaults target Apple `container` with the pinned image.
RUNTIME = os.environ.get("ORACLE_RUNTIME", "container")
CONTAINER = os.environ.get("ORACLE_CONTAINER", "pg_oracle")
IMAGE = os.environ.get("ORACLE_IMAGE", "postgis/postgis:16-3.4")
SOURCE = "3dbag.city.jsonl"

# Pinned oracle versions (postgis/postgis:16-3.4). A tag move that changes these
# can shift the golden values, so warn loudly on drift (recorded in the CSV too).
# GEOS backs ST_ConvexHull (the hull-area golden), so track it alongside SFCGAL.
EXPECTED_PG, EXPECTED_SFCGAL = "3.4.3", "1.3.8"
EXPECTED_GEOS = "3.9.0-CAPI-1.16.2"

# The planar unit tetrahedron is the harness's canary: SFCGAL must accept it and
# reproduce these exact analytic values. If it doesn't, the oracle is broken and
# generation must fail rather than freeze every row as 'rejected'.
CANARY = "fixture:tetra"
# Tetra (0,0,0),(1,0,0),(0,1,0),(0,0,1): three right-triangle faces of area 1/2
# plus the equilateral hypotenuse face of area sqrt(3)/2 -> 3·0.5 + sqrt(3)/2.
CANARY_AREA = 2.3660254037844384
CANARY_VOLUME = 1.0 / 6.0

# Reject anything but hex in wkb_hex and a conservative id charset before it is
# interpolated into the INSERT below (defence in depth; inputs are our own).
_HEX = re.compile(r"\A[0-9a-fA-F]*\Z")
_SAFE_ID = re.compile(r"\A[\w:.\-]*\Z")

# Columns whose values are floating-point and must be formatted deterministically.
BBOX_COLS = ("pg_min_x", "pg_min_y", "pg_min_z", "pg_max_x", "pg_max_y", "pg_max_z")
FLOAT_COLS = (("pg_area3d", "pg_volume", "pg_hull_area", "pg_length3d",
               "pg_proj_area", "pg_x", "pg_y", "pg_z") + BBOX_COLS)
PAIR_FLOAT_COLS = ("pg_dist3d", "pg_maxdist3d", "threshold",
                   "pg_shortline_len", "pg_closestpoint_dist")
TRANSFORM_FLOAT_COLS = ("pg_area3d", "pg_volume", "pg_length3d",
                        "pg_proj_area") + BBOX_COLS
FLOAT_FMT = "%.17g"  # round-trippable, stable across runs of the same image

# PostGIS COPY writes booleans as t/f; normalise to true/false so DuckDB's
# read_csv infers a real BOOLEAN column in Phase B.
BOOL_COLS = ("pg_is_closed", "pg_is_planar", "pg_wkb_roundtrip")
PAIR_BOOL_COLS = ("pg_intersects", "pg_dwithin", "pg_dfullywithin")
TRANSFORM_BOOL_COLS = ()
_BOOL = {"t": "true", "f": "false"}

# Geometry pairs for the distance/relation oracle: (feature_a, feature_b,
# threshold). Chosen so every boolean predicate below straddles true and false
# (dist(035935,028278) ≈ 517.6, maxdist ≈ 563.8), keeping Phase B non-vacuous:
#   intersects — false for the two distinct buildings, true for the self-pairs
#   dwithin    — false at 100, true at 1000 / for the self-pairs
#   dfullywithin — false at 100 and for the real self-pair (its diameter > 1),
#                  true at 1000 and for the tetra self-pair (diameter √2 < 2)
PAIRS = (
    ("NL.IMBAG.Pand.0703100000035935-0", "NL.IMBAG.Pand.0703100000028278-0", 1000.0),
    ("NL.IMBAG.Pand.0703100000035935-0", "NL.IMBAG.Pand.0703100000028278-0", 100.0),
    ("NL.IMBAG.Pand.0703100000035935-0", "NL.IMBAG.Pand.0703100000035935-0", 1.0),
    ("fixture:tetra", "fixture:tetra", 2.0),
    # Cross-class pairs over the GEOM_3D fixtures: the distance family dispatches
    # on both operands' classes, and the solid-only pairs above never reach the
    # point/line/polygon branches.
    ("fixture:point", "fixture:polygon", 5.0),
    ("fixture:line", "fixture:multipolygon", 10.0),
    ("fixture:multipoint", "fixture:multiline", 20.0),
)


def run(cmd: list[str], *, stdin: str | None = None, env: dict[str, str] | None = None) -> str:
    """Run a subprocess, return stdout, raise with stderr on failure."""
    proc = subprocess.run(
        cmd,
        input=stdin,
        capture_output=True,
        text=True,
        env=env,
    )
    if proc.returncode != 0:
        sys.stderr.write(proc.stderr)
        raise SystemExit(f"command failed ({proc.returncode}): {' '.join(cmd)}")
    return proc.stdout


def psql(sql: str, *, quiet: bool = True) -> str:
    """Run SQL in the oracle container via `<runtime> exec -i <name> psql`."""
    args = [RUNTIME, "exec", "-i", CONTAINER, "psql", "-U", "postgres",
            "-v", "ON_ERROR_STOP=1"]
    if quiet:
        args.append("-q")  # suppress command tags so COPY TO STDOUT is clean
    return run(args, stdin=sql)


INPUT_COLS = ("feature_id", "lod", "geom_role", "wkb_hex")


def export_wkb_inputs() -> list[dict[str, str]]:
    """DuckDB side: (feature_id, lod, geom_role, wkb_hex) rows from the fixture."""
    if not DUCKDB.exists():
        raise SystemExit(f"release CLI not found at {DUCKDB}; run `just build` first")
    if not Path(CITYJSON_EXTENSION).exists():
        raise SystemExit(
            f"cityjson extension not found at {CITYJSON_EXTENSION}; build the sibling "
            f"duckdb-cityjson checkout (`GEN=ninja make release`) or set CITYJSON_EXTENSION")
    # export_wkb.sql opens with the analytic ST_AsWKB* fixtures, which three_d
    # only registers when THREE_D_TEST_FIXTURES is set (src/functions/fixtures.cpp).
    out = run([str(DUCKDB), "-unsigned",
               "-cmd", "LOAD three_d;",
               "-cmd", f"LOAD '{CITYJSON_EXTENSION}';",
               "-c", f".read {EXPORT_SQL}"],
              env={**os.environ, "THREE_D_TEST_FIXTURES": "1"})
    return list(csv.DictReader(io.StringIO(out)))


def frozen_wkb_inputs() -> list[dict[str, str]]:
    """Read the frozen (feature_id, lod, geom_role, wkb_hex) from golden.csv."""
    if not OUT_CSV.exists():
        raise SystemExit(f"{OUT_CSV} not found; bootstrap it once with --reexport")
    with OUT_CSV.open() as f:
        return [{c: r[c] for c in INPUT_COLS} for r in csv.DictReader(f)]


def oracle_versions() -> tuple[str, str, str]:
    out = psql("SELECT postgis_lib_version() || '|' || postgis_sfcgal_version() "
               "|| '|' || postgis_geos_version();", quiet=False)
    for line in out.splitlines():
        parts = line.strip().split("|")
        if len(parts) == 3 and "." in line:
            return parts[0], parts[1], parts[2]
    raise SystemExit("could not read PostGIS/SFCGAL/GEOS versions from container")


def _validate(rows: list[dict[str, str]]) -> None:
    for r in rows:
        if not _HEX.match(r["wkb_hex"]):
            raise SystemExit(f"unsafe wkb_hex for {r['feature_id']!r}")
        for c in ("feature_id", "lod", "geom_role"):
            if not _SAFE_ID.match(r[c]):
                raise SystemExit(f"unsafe {c}: {r[c]!r}")


def _load_inputs_sql(rows: list[dict[str, str]]) -> str:
    """SQL that recreates the TEMP wkb_inputs table. TEMP means per-session, so
    every psql invocation that reads it has to reload it."""
    _validate(rows)
    values = ",\n".join(
        "(" + ",".join(_pgquote(r[c]) for c in
                       ("feature_id", "lod", "geom_role", "wkb_hex")) + ")"
        for r in rows
    )
    return (
        "CREATE TEMP TABLE wkb_inputs"
        " (feature_id text, lod text, geom_role text, wkb_hex text);\n"
        f"INSERT INTO wkb_inputs VALUES\n{values};\n"
    )


def run_oracle(rows: list[dict[str, str]]) -> list[dict[str, str]]:
    """Load wkb_inputs into PostGIS, run postgis_oracle.sql, return CSV rows."""
    out = psql(_load_inputs_sql(rows) + ORACLE_SQL.read_text())
    return list(csv.DictReader(io.StringIO(out)))


def run_oracle_transforms(rows: list[dict[str, str]]) -> list[dict[str, str]]:
    """Run postgis_oracle_transforms.sql. Must follow run_oracle: it calls the
    oracle_area3d/oracle_volume wrappers postgis_oracle.sql defines."""
    out = psql(_load_inputs_sql(rows) + ORACLE_TRANSFORMS_SQL.read_text())
    return list(csv.DictReader(io.StringIO(out)))


def _pgquote(s: str) -> str:
    return "'" + s.replace("'", "''") + "'"


def check_canary(rows: list[dict[str, str]]) -> None:
    """Fail generation if SFCGAL didn't reproduce the tetra fixture exactly.

    Guards against the EXCEPTION-WHEN-others wrappers silently masking a broken
    oracle (missing SFCGAL, wrong image) as blanket 'rejected' rows — which would
    otherwise produce a useless golden.csv that the CI test can't distinguish.
    """
    row = next((r for r in rows if r["feature_id"] == CANARY), None)
    if row is None:
        raise SystemExit(f"sanity: canary {CANARY!r} missing from oracle output")
    if row["pg_area_status"] != "ok" or row["pg_volume_status"] != "ok":
        raise SystemExit(
            f"sanity: SFCGAL rejected the planar {CANARY} — oracle likely broken "
            f"(area={row['pg_area_status']}, volume={row['pg_volume_status']})")
    if not math.isclose(float(row["pg_area3d"]), CANARY_AREA, rel_tol=1e-9):
        raise SystemExit(f"sanity: {CANARY} area {row['pg_area3d']} != {CANARY_AREA}")
    if not math.isclose(float(row["pg_volume"]), CANARY_VOLUME, rel_tol=1e-9):
        raise SystemExit(f"sanity: {CANARY} volume {row['pg_volume']} != {CANARY_VOLUME}")
    # The serialization column is a self-comparison inside PostGIS; if it were
    # ever uniformly false the Phase B assertion built on it would be vacuous.
    if row["pg_wkb_roundtrip"] != "t":
        raise SystemExit(f"sanity: PostGIS did not re-emit {CANARY}'s input WKB unchanged")


def run_oracle_pairs(pairs, wkb_by_id: dict[str, str]) -> list[dict[str, str]]:
    """Load pair_inputs into PostGIS, run postgis_oracle_pairs.sql, return rows."""
    for a, b, _ in pairs:
        for fid in (a, b):
            if fid not in wkb_by_id:
                raise SystemExit(f"pair references unknown feature {fid!r}")
    values = ",\n".join(
        "(" + ",".join([_pgquote(a), _pgquote(b),
                        _pgquote(wkb_by_id[a]), _pgquote(wkb_by_id[b]),
                        repr(float(thr))]) + ")"
        for a, b, thr in pairs
    )
    load = (
        "CREATE TEMP TABLE pair_inputs (feature_a text, feature_b text,"
        " wkb_a text, wkb_b text, threshold double precision);\n"
        f"INSERT INTO pair_inputs VALUES\n{values};\n"
    )
    out = psql(load + ORACLE_PAIRS_SQL.read_text())
    return list(csv.DictReader(io.StringIO(out)))


def check_pair_canary(rows: list[dict[str, str]]) -> None:
    """The tetra self-pair must have distance 0 and intersect — cheap proof the
    distance/relation oracle actually computed rather than silently degenerating."""
    row = next((r for r in rows if r["feature_a"] == CANARY
                and r["feature_b"] == CANARY), None)
    if row is None:
        raise SystemExit(f"sanity: pair canary {CANARY}/{CANARY} missing")
    if not math.isclose(float(row["pg_dist3d"]), 0.0, abs_tol=1e-9):
        raise SystemExit(f"sanity: {CANARY} self-distance {row['pg_dist3d']} != 0")
    if row["pg_intersects"] != "t":
        raise SystemExit(f"sanity: {CANARY} self-pair not intersecting")


def check_transform_canary(rows: list[dict[str, str]]) -> None:
    """The transform oracle must reproduce the analytic invariants on the tetra:
    a rotation preserves volume exactly, the anisotropic scale multiplies it by
    det = 2·3·0.5 = 3. Without this a silently degenerate op (e.g. an identity
    ST_RotateX) would freeze values Phase B then happily agrees with."""
    by_op = {r["op"]: r for r in rows if r["feature_id"] == CANARY}
    for op in ("translate", "scale", "rotatex", "rotatey", "rotatez"):
        if op not in by_op:
            raise SystemExit(f"sanity: transform canary {CANARY}/{op} missing")
        if by_op[op]["pg_volume_status"] != "ok":
            raise SystemExit(f"sanity: SFCGAL rejected the {op}d {CANARY}")
    for op in ("translate", "rotatex", "rotatey", "rotatez"):
        if not math.isclose(float(by_op[op]["pg_volume"]), CANARY_VOLUME, rel_tol=1e-9):
            raise SystemExit(
                f"sanity: {op} changed {CANARY}'s volume ({by_op[op]['pg_volume']})")
    if not math.isclose(float(by_op["scale"]["pg_volume"]), 3.0 * CANARY_VOLUME,
                        rel_tol=1e-9):
        raise SystemExit(
            f"sanity: scale did not triple {CANARY}'s volume ({by_op['scale']['pg_volume']})")
    # A translation must move the bbox and leave its extent alone; catches an op
    # column that silently carried the untransformed geometry through.
    if float(by_op["translate"]["pg_min_x"]) != 100.5:
        raise SystemExit("sanity: translate did not move the canary bbox")


def format_golden(rows: list[dict[str, str]], pg_ver: str, sfcgal_ver: str,
                  geos_ver: str, *, float_cols, bool_cols, sort_key) -> str:
    """Deterministic CSV: sorted rows, fixed float format, provenance columns."""
    fieldnames = list(rows[0].keys()) + ["source", "pg_version", "sfcgal_version",
                                         "geos_version"]
    for r in rows:
        for c in float_cols:
            if r.get(c) not in (None, ""):
                r[c] = FLOAT_FMT % float(r[c])
        for c in bool_cols:
            if r.get(c) in _BOOL:
                r[c] = _BOOL[r[c]]
        r["source"] = SOURCE
        r["pg_version"] = pg_ver
        r["sfcgal_version"] = sfcgal_ver
        r["geos_version"] = geos_ver
    rows.sort(key=sort_key)
    buf = io.StringIO()
    w = csv.DictWriter(buf, fieldnames=fieldnames, lineterminator="\n")
    w.writeheader()
    w.writerows(rows)
    return buf.getvalue()


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--reexport", action="store_true",
                    help="re-derive wkb_hex from the fixture via DuckDB+cityjson "
                         "(needs a published cityjson); default reuses frozen WKB")
    args = ap.parse_args()

    inputs = export_wkb_inputs() if args.reexport else frozen_wkb_inputs()
    if not inputs:
        raise SystemExit("no WKB inputs")
    # run_oracle runs postgis_oracle.sql's CREATE EXTENSION first, so query the
    # versions afterwards — on a fresh container SFCGAL isn't registered yet.
    oracle_rows = run_oracle(inputs)
    check_canary(oracle_rows)
    pair_rows = run_oracle_pairs(PAIRS, {r["feature_id"]: r["wkb_hex"] for r in inputs})
    check_pair_canary(pair_rows)
    transform_rows = run_oracle_transforms(inputs)
    check_transform_canary(transform_rows)
    pg_ver, sfcgal_ver, geos_ver = oracle_versions()
    if (pg_ver, sfcgal_ver, geos_ver) != (EXPECTED_PG, EXPECTED_SFCGAL, EXPECTED_GEOS):
        sys.stderr.write(
            f"warning: oracle versions {pg_ver}/{sfcgal_ver}/{geos_ver} differ from "
            f"pinned {EXPECTED_PG}/{EXPECTED_SFCGAL}/{EXPECTED_GEOS}; values may shift\n")
    OUT_CSV.write_text(format_golden(
        oracle_rows, pg_ver, sfcgal_ver, geos_ver,
        float_cols=FLOAT_COLS, bool_cols=BOOL_COLS,
        sort_key=lambda r: (r["geom_role"], r["feature_id"], r["lod"])))
    OUT_PAIRS_CSV.write_text(format_golden(
        pair_rows, pg_ver, sfcgal_ver, geos_ver,
        float_cols=PAIR_FLOAT_COLS, bool_cols=PAIR_BOOL_COLS,
        sort_key=lambda r: (r["feature_a"], r["feature_b"], float(r["threshold"]))))
    OUT_TRANSFORMS_CSV.write_text(format_golden(
        transform_rows, pg_ver, sfcgal_ver, geos_ver,
        float_cols=TRANSFORM_FLOAT_COLS, bool_cols=TRANSFORM_BOOL_COLS,
        sort_key=lambda r: (r["geom_role"], r["feature_id"], r["op"])))
    mode = "re-export" if args.reexport else "frozen-WKB"
    print(f"wrote {OUT_CSV.relative_to(REPO)}: {len(oracle_rows)} rows; "
          f"{OUT_PAIRS_CSV.name}: {len(pair_rows)} rows; "
          f"{OUT_TRANSFORMS_CSV.name}: {len(transform_rows)} rows "
          f"[{mode}] (PostGIS {pg_ver}, SFCGAL {sfcgal_ver}, GEOS {geos_ver})")


if __name__ == "__main__":
    main()
