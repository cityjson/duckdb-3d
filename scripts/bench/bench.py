#!/usr/bin/env python3
"""Micro-benchmark of the three_d SQL surface on real CityJSON data.

Usage:
    scripts/bench/prepare.sh <dir>        # once: download + materialise inputs
    scripts/bench/bench.py --inputs <dir>/inputs.duckdb \
        --duckdb build/release/duckdb[,/path/to/other/duckdb] [--reps 3] [--rounds 3]

`inputs.duckdb` holds one table per dataset (3DBAG Delft LoD 2.2, Helsinki
LoD 2) with columns (id, wkb BLOB, props STRUCT, props_json VARCHAR), so the
timings never include CityJSON parsing.

Each binary runs in its own CLI process (the shell built with three_d linked
in): the setup statements materialise the solids, geometries and pair sets,
then every query runs `reps` times under `.timer on`. `rounds` repeats that,
alternating the binaries, so drifting machine load hits all of them alike.
Reported per query: median wall and CPU (user + sys) seconds per binary, and
an order-independent digest of the exact per-row results -- a hash sum over
the payload bytes or doubles -- so comparing two builds also proves they
return bit-identical answers ("DIFF" otherwise).
"""
import argparse
import re
import statistics
import subprocess
import sys

DATASETS = ["delft", "helsinki"]

# Pair sets for the distance family: buildings whose bbox centres share a
# 100 m grid cell, capped so the brute-force distance stays measurable.
PAIR_LIMIT = {"delft": 5000, "helsinki": 5000}


def setup_sql(ds):
    lim = PAIR_LIMIT[ds]
    return f"""
CREATE TEMP TABLE {ds} AS SELECT * FROM i.{ds};
CREATE TEMP TABLE {ds}_s AS SELECT id, ST_3DTryFromWKB(wkb, props) AS s FROM {ds};
CREATE TEMP TABLE {ds}_valid AS SELECT s FROM {ds}_s WHERE s IS NOT NULL AND (ST_3DValidationReport(s)).is_valid;
CREATE TEMP TABLE {ds}_nondegen AS SELECT s FROM {ds}_s WHERE s IS NOT NULL AND (ST_3DValidationReport(s)).degenerate_face_count = 0;
CREATE TEMP TABLE {ds}_g AS SELECT id, ST_Geom3DFromWKB(wkb) AS g,
    ((b).min_x + (b).max_x) / 2 AS cx, ((b).min_y + (b).max_y) / 2 AS cy
    FROM (SELECT id, wkb, ST_3DBounds(ST_3DTryFromWKB(wkb, props)) AS b FROM {ds}) WHERE b IS NOT NULL;
CREATE TEMP TABLE {ds}_pairs AS
    SELECT a.g AS g1, b.g AS g2 FROM {ds}_g a JOIN {ds}_g b
      ON floor(a.cx / 100) = floor(b.cx / 100) AND floor(a.cy / 100) = floor(b.cy / 100) AND a.id < b.id
    ORDER BY a.id, b.id LIMIT {lim};
"""


def bench_queries(ds):
    # Every query returns an order-independent digest of the exact per-row
    # results (sum of 64-bit hashes), so a before/after run proves identical
    # payload bytes and bit-identical doubles, not just equal rounded sums.
    h = lambda e: f"sum(hash({e})::HUGEINT)"
    return [
        ("import_struct", f"SELECT {h('ST_3DTryFromWKB(wkb, props)')} FROM {ds};"),
        ("import_json", f"SELECT {h('ST_3DTryFromWKB(wkb, props_json)')} FROM {ds};"),
        ("import_plain", f"SELECT {h('ST_3DTryFromWKB(wkb)')} FROM {ds};"),
        ("geom_import", f"SELECT {h('ST_Geom3DFromWKB(wkb)')} FROM {ds};"),
        ("volume", f"SELECT sum(v)::DECIMAL(38,6), {h('v')} FROM (SELECT ST_3DVolume(s) AS v FROM {ds}_valid);"),
        ("area", f"SELECT sum(v)::DECIMAL(38,6), {h('v')} FROM (SELECT ST_3DArea(s) AS v FROM {ds}_nondegen);"),
        (
            "footprint",
            f"SELECT sum(v)::DECIMAL(38,6), {h('v')} FROM (SELECT ST_3DFootprintArea(s) AS v FROM {ds}_nondegen);",
        ),
        ("validation_report", f"SELECT {h('ST_3DValidationReport(s)')} FROM {ds}_s WHERE s IS NOT NULL;"),
        (
            "distance",
            f"SELECT sum(v)::DECIMAL(38,6), {h('v')} FROM (SELECT ST_3DDistance(g1, g2) AS v FROM {ds}_pairs);",
        ),
        ("dwithin_2m", f"SELECT count_if(v), {h('v')} FROM (SELECT ST_3DDWithin(g1, g2, 2.0) AS v FROM {ds}_pairs);"),
        ("shortestline", f"SELECT {h('ST_3DShortestLine(g1, g2)')} FROM {ds}_pairs;"),
    ]


def build_script(inputs, reps, threads, datasets, only=None):
    out = [f"ATTACH '{inputs}' AS i (READ_ONLY);", f"SET threads = {threads};"]
    for ds in datasets:
        out.append(setup_sql(ds))
    out.append(".timer on")
    for ds in datasets:
        for name, q in bench_queries(ds):
            if only and name not in only:
                continue
            for r in range(reps):
                out.append(f".print @@BENCH {ds} {name} {r}")
                out.append(q)
    return "\n".join(out) + "\n"


TIME_RE = re.compile(r"Run Time \(s\): real ([0-9.]+) user ([0-9.]+) sys ([0-9.]+)")


def run(binary, script):
    proc = subprocess.run([binary, "-unsigned", "-csv", "-noheader"], input=script, capture_output=True, text=True)
    if proc.returncode != 0:
        sys.stderr.write(proc.stderr)
        raise SystemExit(f"{binary} failed")
    results = {}
    cur = None
    for line in proc.stdout.splitlines():
        if line.startswith("@@BENCH "):
            _, ds, name, r = line.split()
            cur = (ds, name)
            results.setdefault(cur, {"times": [], "cpu": [], "values": set()})
            continue
        if cur is None:
            continue
        m = TIME_RE.search(line)
        if m:
            results[cur]["times"].append(float(m.group(1)))
            results[cur]["cpu"].append(float(m.group(2)) + float(m.group(3)))
        elif line.strip():
            results[cur]["values"].add(line.strip())
    return results


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--inputs", required=True)
    ap.add_argument("--duckdb", required=True, help="comma-separated binaries (three_d linked in)")
    ap.add_argument("--reps", type=int, default=3)
    ap.add_argument("--threads", type=int, default=1)
    ap.add_argument("--rounds", type=int, default=1)
    ap.add_argument("--datasets", default=",".join(DATASETS))
    ap.add_argument("--queries", default="", help="comma-separated subset of query names")
    args = ap.parse_args()
    datasets = args.datasets.split(",")
    script = build_script(args.inputs, args.reps, args.threads, datasets, set(filter(None, args.queries.split(","))))
    binaries = args.duckdb.split(",")
    # Rounds alternate the binaries so drift in machine load hits all of them
    # alike; samples from every round are pooled before taking the median.
    all_results = [{} for _ in binaries]
    for _ in range(args.rounds):
        for k, b in enumerate(binaries):
            for key, r in run(b, script).items():
                acc = all_results[k].setdefault(key, {"times": [], "cpu": [], "values": set()})
                acc["times"] += r["times"]
                acc["cpu"] += r["cpu"]
                acc["values"] |= r["values"]
    header = (
        ["dataset", "query"]
        + [f"wall[{i}]" for i in range(len(binaries))]
        + [f"cpu[{i}]" for i in range(len(binaries))]
        + ["result"]
    )
    print("	".join(header))
    for key in all_results[0]:
        walls = [statistics.median(r[key]["times"]) for r in all_results]
        cpus = [statistics.median(r[key]["cpu"]) for r in all_results]
        vals = [";".join(sorted(r[key]["values"])) for r in all_results]
        same = len(set(vals)) == 1
        print(
            "	".join(
                [key[0], key[1]]
                + [f"{m:.4f}" for m in walls]
                + [f"{m:.4f}" for m in cpus]
                + [vals[0] if same else "DIFF " + " | ".join(vals)]
            )
        )


if __name__ == "__main__":
    main()
