"""live_diff.py: compare duckdb-3d with a live PostGIS + SFCGAL on real city models.

Dev-time only, like gen_golden.py: CI keeps comparing against the frozen goldens in
test/data/postgis_oracle/, and nothing here runs under `make test`. This harness is
what a change to a function is verified with before it is committed: it reads
published CityJSON / CityJSONSeq files, builds the same ISO WKB bytes for both
engines, runs each check on both and reports every disagreement beyond the check's
tolerance.

The WKB is built here, in Python, straight from the CityJSON boundaries, so neither
engine's reader is part of the comparison: a disagreement is about the maths.

    python3 scripts/oracle/live_diff.py --checks distance --report out.md

Needs: a release build (build/release/extension/three_d/three_d.duckdb_extension),
the `duckdb` Python package at the build's DuckDB version, `psycopg2`, and a
PostgreSQL database with the postgis and postgis_sfcgal extensions (--dsn).
Downloads are cached in --workdir and pinned by sha256.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import random
import struct
import sys
import urllib.request
from dataclasses import dataclass, field
from pathlib import Path

import duckdb
import psycopg2

REPO = Path(__file__).resolve().parents[2]
EXTENSION = REPO / "build" / "release" / "extension" / "three_d" / "three_d.duckdb_extension"

# Published datasets, pinned by sha256 (the same pins as the CityParquet tool-chain
# corpus). A mismatch is an error: data that changed under the test changes what
# the report means.
DATASETS = {
    "delft": ("https://cityjson.open3d.city/cityjsonseq/delft.city.jsonl",
              "2f3d95698bf5871999aac04744fc74a7de0609e88d9e5af6a1dd5fd050635067"),
    "railway": ("https://3d.bk.tudelft.nl/opendata/cityjson/3dcities/v2.0/LoD3_Railway.city.json",
                "60a80c34d50e167556d858c4fe3b4b11b297c3b450c7785db289e90dcc289b4e"),
    "rotterdam": ("https://3d.bk.tudelft.nl/opendata/cityjson/3dcities/v2.0/3-20-DELFSHAVEN.city.json",
                  "958460c670b4cb85a9159d1e5e566905fba8812864f64b554ecad38333badcf6"),
    "denhaag": ("https://3d.bk.tudelft.nl/opendata/cityjson/3dcities/v2.0/DenHaag_01.city.json",
                "7d500b53f7e1e7dfc8f6bd0b2b4ab26c06a27b71b86ec1005f05f1d35afb17f8"),
    "vienna": ("https://3d.bk.tudelft.nl/opendata/cityjson/3dcities/v2.0/Vienna_102081.city.json",
               "849a148b5fb91ebfa6e3efe7bd78be47cc5cdd2fb6699e0cb042f44dd702cc62"),
}


# --------------------------------------------------------------------------
# Reading CityJSON into WKB
# --------------------------------------------------------------------------

@dataclass
class Item:
    """One geometry under test."""
    key: str          # unique: dataset/object/lod/geometry index[/face]
    dataset: str
    object_id: str
    object_type: str
    lod: str
    role: str         # "solid", "surface" or "face"
    wkb: bytes
    shells: list[int] = field(default_factory=list)  # per-shell face counts (solids)
    attributes: dict = field(default_factory=dict)
    bbox: tuple = ()
    faces: list = field(default_factory=list)   # [[ring of (x, y, z)]], for the independent oracle
    holes: bool = False                          # some face has an interior ring
    deviation: float = 0.0                       # largest distance of a vertex from its face's plane


def _polygon(face, verts) -> bytes:
    out = bytearray(struct.pack("<BII", 1, 1003, len(face)))
    for ring in face:
        out += struct.pack("<I", len(ring) + 1)
        for i in list(ring) + [ring[0]]:
            out += struct.pack("<ddd", *verts[i])
    return bytes(out)


def _surface(faces, verts) -> bytes:
    out = bytearray(struct.pack("<BII", 1, 1015, len(faces)))
    for face in faces:
        out += _polygon(face, verts)
    return bytes(out)


def _bbox(faces, verts) -> tuple:
    pts = [verts[i] for face in faces for ring in face for i in ring]
    xs, ys, zs = zip(*pts)
    return (min(xs), min(ys), min(zs), max(xs), max(ys), max(zs))


def _features(path: Path):
    """Yield (vertices, CityObjects) per feature, for CityJSON and CityJSONSeq."""
    with open(path) as f:
        first = json.loads(f.readline() if path.suffix == ".jsonl" else f.read())
        tr = first.get("transform", {"scale": [1, 1, 1], "translate": [0, 0, 0]})
        s, t = tr["scale"], tr["translate"]

        def scaled(vs):
            return [(v[0] * s[0] + t[0], v[1] * s[1] + t[1], v[2] * s[2] + t[2]) for v in vs]

        if path.suffix == ".jsonl":
            for line in f:
                feat = json.loads(line)
                yield scaled(feat["vertices"]), feat["CityObjects"]
        else:
            yield scaled(first["vertices"]), first["CityObjects"]


def _newell(ring):
    """Unnormalised Newell normal of a ring, about its first vertex."""
    o = ring[0]
    nx = ny = nz = 0.0
    for a, b in zip(ring, ring[1:] + ring[:1]):
        ax, ay, az = a[0] - o[0], a[1] - o[1], a[2] - o[2]
        bx, by, bz = b[0] - o[0], b[1] - o[1], b[2] - o[2]
        nx += (ay - by) * (az + bz)
        ny += (az - bz) * (ax + bx)
        nz += (ax - bx) * (ay + by)
    return nx, ny, nz


def face_plane(face):
    """(unit normal, centroid) of a face: the exterior ring's Newell normal through its centroid."""
    ext = face[0]
    nx, ny, nz = _newell(ext)
    length = math.sqrt(nx * nx + ny * ny + nz * nz)
    if length == 0.0:
        return None
    pts = [v for ring in face for v in ring]
    c = tuple(sum(v[k] for v in pts) / len(pts) for k in range(3))
    return (nx / length, ny / length, nz / length), c


def face_deviation(face) -> float:
    plane = face_plane(face)
    if plane is None:
        return 0.0
    n, c = plane
    return max(abs((v[0] - c[0]) * n[0] + (v[1] - c[1]) * n[1] + (v[2] - c[2]) * n[2]) for ring in face for v in ring)


def _coords(faces, verts):
    return [[[tuple(verts[i]) for i in ring] for ring in face] for face in faces]


def _item(key, name, oid, co, lod, role, wkb, shells, attributes, faces, verts) -> Item:
    coords = _coords(faces, verts)
    return Item(key, name, oid, co["type"], lod, role, wkb, shells, attributes, _bbox(faces, verts), coords,
                any(len(f) > 1 for f in coords), max((face_deviation(f) for f in coords), default=0.0))


def read_dataset(name: str, path: Path, max_faces_per_dataset: int) -> list[Item]:
    items: list[Item] = []
    faces_taken = 0
    for verts, objects in _features(path):
        for oid, co in objects.items():
            for gi, g in enumerate(co.get("geometry", [])):
                lod = str(g.get("lod"))
                key = f"{name}/{oid}/{lod}/{gi}"
                if g["type"] == "Solid":
                    faces = [face for shell in g["boundaries"] for face in shell]
                    items.append(_item(key, name, oid, co, lod, "solid", _surface(faces, verts),
                                       [len(shell) for shell in g["boundaries"]], co.get("attributes", {}),
                                       faces, verts))
                elif g["type"] in ("MultiSurface", "CompositeSurface"):
                    faces = g["boundaries"]
                    if not faces:
                        continue
                    items.append(_item(key, name, oid, co, lod, "surface", _surface(faces, verts), [],
                                       co.get("attributes", {}), faces, verts))
                else:
                    continue
                if faces_taken < max_faces_per_dataset:
                    for fi, face in enumerate(faces):
                        if faces_taken >= max_faces_per_dataset:
                            break
                        items.append(_item(f"{key}/{fi}", name, oid, co, lod, "face", _polygon(face, verts), [],
                                           {}, [face], verts))
                        faces_taken += 1
    return items


def fetch(name: str, workdir: Path) -> Path:
    url, sha = DATASETS[name]
    path = workdir / url.rsplit("/", 1)[1]
    if not path.exists():
        print(f"fetching {url}", file=sys.stderr)
        urllib.request.urlretrieve(url, path)
    digest = hashlib.sha256(path.read_bytes()).hexdigest()
    if digest != sha:
        raise SystemExit(f"{path}: sha256 {digest} does not match the pinned {sha}")
    return path


# --------------------------------------------------------------------------
# Independent oracle: distance between planar polygons with holes
# --------------------------------------------------------------------------
#
# PostGIS is not an oracle for faces with holes: its 3D point-in-polygon test
# ignores interior rings (a point at the centre of a 1 x 1 hole in a face gets
# 2.12 instead of 0.5). This is a separate, deliberately plain computation used
# for those rows. A face is taken as its rings projected onto its plane (the
# exterior ring's Newell normal through the centroid), which is exact for
# planar faces. Two compact planar regions are either intersecting (distance 0)
# or their closest pair has a vertex of one against the other, or an edge
# against an edge; nothing else needs testing.

def _sub(a, b):
    return (a[0] - b[0], a[1] - b[1], a[2] - b[2])


def _dot(a, b):
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]


def _seg_seg(p1, q1, p2, q2) -> float:
    """Distance between segments p1q1 and p2q2 (Ericson, Real-Time Collision Detection, 5.1.9)."""
    d1, d2, r = _sub(q1, p1), _sub(q2, p2), _sub(p1, p2)
    a, e, f = _dot(d1, d1), _dot(d2, d2), _dot(d2, r)
    if a == 0.0 and e == 0.0:
        return math.dist(p1, p2)
    if a == 0.0:
        s, t = 0.0, min(1.0, max(0.0, f / e))
    else:
        c = _dot(d1, r)
        if e == 0.0:
            s, t = min(1.0, max(0.0, -c / a)), 0.0
        else:
            b = _dot(d1, d2)
            denom = a * e - b * b
            s = min(1.0, max(0.0, (b * f - c * e) / denom)) if denom != 0.0 else 0.0
            t = (b * s + f) / e
            if t < 0.0:
                t, s = 0.0, min(1.0, max(0.0, -c / a))
            elif t > 1.0:
                t, s = 1.0, min(1.0, max(0.0, (b - c) / a))
    c1 = (p1[0] + d1[0] * s, p1[1] + d1[1] * s, p1[2] + d1[2] * s)
    c2 = (p2[0] + d2[0] * t, p2[1] + d2[1] * t, p2[2] + d2[2] * t)
    return math.dist(c1, c2)


class OracleFace:
    def __init__(self, face):
        self.rings = face
        self.edges = [(ring[i], ring[(i + 1) % len(ring)]) for ring in face for i in range(len(ring))]
        self.plane = face_plane(face)
        pts = [v for ring in face for v in ring]
        self.box = tuple(min(v[k] for v in pts) for k in range(3)) + tuple(max(v[k] for v in pts) for k in range(3))
        if self.plane is not None:
            n, c = self.plane
            u = (1.0, 0.0, 0.0) if abs(n[0]) < 0.9 else (0.0, 1.0, 0.0)
            u = _sub(u, tuple(n[k] * _dot(u, n) for k in range(3)))
            lu = math.sqrt(_dot(u, u))
            self.u = tuple(x / lu for x in u)
            self.v = (n[1] * self.u[2] - n[2] * self.u[1], n[2] * self.u[0] - n[0] * self.u[2],
                      n[0] * self.u[1] - n[1] * self.u[0])
            self.rings2 = [[self._to2(p) for p in ring] for ring in face]

    def _to2(self, p):
        d = _sub(p, self.plane[1])
        return (_dot(d, self.u), _dot(d, self.v))

    def contains_projection(self, p) -> bool:
        """Even-odd test of p's projection against every ring: holes excluded."""
        x, y = self._to2(p)
        inside = False
        for ring in self.rings2:
            for i in range(len(ring)):
                (x1, y1), (x2, y2) = ring[i], ring[(i + 1) % len(ring)]
                if (y1 > y) != (y2 > y) and x < x1 + (y - y1) * (x2 - x1) / (y2 - y1):
                    inside = not inside
        return inside

    def point_distance(self, p) -> float:
        best = min(_seg_seg(p, p, a, b) for a, b in self.edges)
        if self.plane is not None and self.contains_projection(p):
            n, c = self.plane
            best = min(best, abs(_dot(_sub(p, c), n)))
        return best

    def crossed_by(self, a, b) -> bool:
        """Does segment ab pass through the face's interior?"""
        if self.plane is None:
            return False
        n, c = self.plane
        sa, sb = _dot(_sub(a, c), n), _dot(_sub(b, c), n)
        if (sa > 0 and sb > 0) or (sa < 0 and sb < 0) or sa == sb:
            return False
        t = sa / (sa - sb)
        return self.contains_projection((a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t,
                                         a[2] + (b[2] - a[2]) * t))


def _box_gap(a, b) -> float:
    return math.sqrt(sum(max(0.0, a[i] - b[i + 3], b[i] - a[i + 3]) ** 2 for i in range(3)))


def oracle_face_face(f: OracleFace, g: OracleFace) -> float:
    if any(g.crossed_by(a, b) for a, b in f.edges) or any(f.crossed_by(a, b) for a, b in g.edges):
        return 0.0
    best = min(min(g.point_distance(v) for ring in f.rings for v in ring),
               min(f.point_distance(v) for ring in g.rings for v in ring))
    for a, b in f.edges:
        for c, d in g.edges:
            best = min(best, _seg_seg(a, b, c, d))
    return best


def oracle_distance(fa: list, fb: list) -> float:
    """Distance between two sets of OracleFace (a point is a list with one point tuple)."""
    best = math.inf
    for f in fa:
        for g in fb:
            if _box_gap(f.box, g.box) >= best:
                continue
            best = min(best, oracle_face_face(f, g))
            if best == 0.0:
                return 0.0
    return best


def oracle_max_distance(fa: list, pb: list) -> float:
    """Largest distance between the vertices of every ring of `fa` (faces as
    lists of rings) and the points `pb`. On polygons the maximum is attained at
    vertices, interior rings included: real data has holes that leave their
    exterior, and PostGIS's ST_3DMaxDistance reads only the exterior ring."""
    va = [v for face in fa for ring in face for v in ring]
    return max(math.dist(u, v) for u in va for v in pb)


def oracle_point_distance(p, faces: list) -> float:
    best = math.inf
    for f in faces:
        if _box_gap(f.box, p + p) >= best:
            continue
        best = min(best, f.point_distance(p))
    return best


# --------------------------------------------------------------------------
# Engines
# --------------------------------------------------------------------------

class Engines:
    def __init__(self, extension: Path, dsn: str):
        self.duck = duckdb.connect(config={"allow_unsigned_extensions": "true"})
        self.duck.execute(f"LOAD '{extension}'")
        self.pg = psycopg2.connect(dsn)
        self.pg.autocommit = True
        with self.pg.cursor() as cur:
            cur.execute("CREATE EXTENSION IF NOT EXISTS postgis")
            cur.execute("CREATE EXTENSION IF NOT EXISTS postgis_sfcgal")
            cur.execute("SELECT postgis_full_version()")
            self.pg_version = cur.fetchone()[0]

    def load(self, table: str, columns: list[tuple[str, str, str]], rows: list[tuple]):
        """Create `table` in both engines from `rows`; columns are (name, duck type, pg type)."""
        self.duck.execute(f"DROP TABLE IF EXISTS {table}")
        self.duck.execute(f"CREATE TABLE {table} ({', '.join(f'{n} {d}' for n, d, _ in columns)})")
        if rows:
            self.duck.executemany(f"INSERT INTO {table} VALUES ({', '.join('?' for _ in columns)})", rows)
        with self.pg.cursor() as cur:
            cur.execute(f"DROP TABLE IF EXISTS {table}")
            cur.execute(f"CREATE TABLE {table} ({', '.join(f'{n} {p}' for n, _, p in columns)})")
            if rows:
                psycopg2.extras.execute_values(
                    cur, f"INSERT INTO {table} VALUES %s",
                    [tuple(psycopg2.Binary(v) if isinstance(v, (bytes, bytearray)) else v for v in r) for r in rows],
                    page_size=500)

    def duck_rows(self, sql: str) -> dict:
        return {r[0]: r[1:] for r in self.duck.execute(sql).fetchall()}

    def pg_rows(self, sql: str) -> dict:
        with self.pg.cursor() as cur:
            cur.execute(sql)
            return {r[0]: r[1:] for r in cur.fetchall()}


# --------------------------------------------------------------------------
# Checks
# --------------------------------------------------------------------------

@dataclass
class Outcome:
    check: str
    compared: int = 0
    agree: int = 0
    skipped: dict = field(default_factory=dict)   # reason -> count
    disagreements: list = field(default_factory=list)  # (key, ours, theirs, note)
    max_abs: float = 0.0
    oracle: str = "PostGIS"
    note: str = ""
    informational: bool = False  # reported, but a disagreement does not fail the run


def close(a, b, abs_tol, rel_tol) -> bool:
    if a is None or b is None:
        return a is b
    if isinstance(a, bool) or isinstance(b, bool):
        return a == b
    if math.isinf(a) or math.isinf(b):
        return a == b
    return abs(a - b) <= abs_tol + rel_tol * max(abs(a), abs(b))


def compare(outcome: Outcome, ours: dict, theirs: dict, abs_tol=1e-6, rel_tol=1e-9, labels=None):
    for key, their_vals in theirs.items():
        if key not in ours:
            outcome.skipped["missing on duckdb-3d side"] = outcome.skipped.get("missing on duckdb-3d side", 0) + 1
            continue
        our_vals = ours[key]
        outcome.compared += 1
        ok = True
        for i, (a, b) in enumerate(zip(our_vals, their_vals)):
            if isinstance(a, float) and isinstance(b, float) and not (math.isnan(a) or math.isnan(b)):
                outcome.max_abs = max(outcome.max_abs, abs(a - b))
            if not close(a, b, abs_tol, rel_tol):
                ok = False
        if ok:
            outcome.agree += 1
        else:
            outcome.disagreements.append((key, our_vals, their_vals))


def bbox_gap(a: tuple, b: tuple) -> float:
    d2 = 0.0
    for i in range(3):
        g = max(0.0, a[i] - b[i + 3], b[i] - a[i + 3])
        d2 += g * g
    return math.sqrt(d2)


def point_wkb(x, y, z) -> bytes:
    return struct.pack("<BIddd", 1, 1001, x, y, z)


def near_pairs(items: list[Item], within: float, limit: int, rng: random.Random) -> list[tuple[Item, Item]]:
    """Pairs of distinct objects of the same dataset, LoD and role whose boxes come within `within`."""
    groups: dict = {}
    for it in items:
        groups.setdefault((it.dataset, it.lod, it.role), []).append(it)
    pairs = []
    for group in groups.values():
        group.sort(key=lambda it: it.bbox[0])
        for i, a in enumerate(group):
            for b in group[i + 1:]:
                if b.bbox[0] - a.bbox[3] > within:
                    break
                if a.object_id != b.object_id and bbox_gap(a.bbox, b.bbox) <= within:
                    pairs.append((a, b))
    rng.shuffle(pairs)
    return pairs[:limit]


PLANAR = 1e-7  # a face whose vertices all lie within this of its plane (m) counts as planar


def _row_class(a: Item, b: Item | None) -> tuple[str, float]:
    """Which reference a row is compared against, and the tolerance that comes with it.

    - "exact": planar faces without holes. PostGIS, to --distance-tol.
    - "holes": a face has an interior ring. The independent oracle above.
    - "non-planar": the independent oracle measures a non-planar face as its rings
      projected onto the Newell plane, duckdb-3d against its triangulation. Every
      triangle lies within the face's deviation of that plane, so the two can differ
      by up to twice the faces' summed deviation. PostGIS is reported beside it
      only for information: it measures against a plane of its own fitting, which
      can sit further from the vertices than the deviation (Delft has faces 0.7 mm
      from planar where PostGIS reports 0 against a true 5 mm).
    """
    dev = a.deviation + (b.deviation if b else 0.0)
    bound = 2.0 * dev if dev > PLANAR else 0.0
    if a.holes or (b is not None and b.holes):
        return "holes", bound
    return ("non-planar" if bound else "exact"), bound


def check_distance(eng: Engines, items: list[Item], args) -> list[Outcome]:
    """The distance family on GEOM_3D: object pairs, faces against faces and
    points, and closest point / shortest line against their definitions."""
    rng = random.Random(args.seed)
    geoms = [it for it in items if it.role in ("solid", "surface", "face")]
    pairs = near_pairs([it for it in geoms if it.role != "face"], args.within, args.pairs, rng)
    face_pairs = near_pairs([it for it in geoms if it.role == "face"], 1.0, args.pairs, rng)
    # Holed faces are rare in LoD2 data; probe all of them, not a sample.
    holed = [it for it in geoms if it.holes]
    rows, meta = [], {}
    for a, b in pairs + face_pairs:
        for t in (0.0, 0.5, 2.0):
            k = f"{a.key}|{b.key}|{t}"
            rows.append((k, a.wkb, b.wkb, t))
            meta[k] = (a, b, None, t)
    # Points around faces: in the face's box, grown by a metre, so many fall in
    # notches and over holes where a wrong decomposition shows. Points in the
    # plane of a holed face, over its holes, are added as well.
    faces = [it for it in geoms if it.role == "face"]
    rng.shuffle(faces)
    probe = faces[: args.points] + [it for it in holed if it.role == "face"]
    for it in probe:
        x0, y0, z0, x1, y1, z1 = it.bbox
        pts = [(rng.uniform(x0 - 1, x1 + 1), rng.uniform(y0 - 1, y1 + 1), rng.uniform(z0 - 1, z1 + 1))
               for _ in range(4)]
        if it.holes:
            for ring in it.faces[0][1:]:
                c = tuple(sum(v[k] for v in ring) / len(ring) for k in range(3))
                pts.append(c)
        for j, p in enumerate(pts):
            k = f"{it.key}|pt{j}|0.5"
            rows.append((k, it.wkb, point_wkb(*p), 0.5))
            meta[k] = (it, None, p, 0.5)
    eng.load("pairs", [("k", "VARCHAR", "text"), ("a", "BLOB", "bytea"), ("b", "BLOB", "bytea"),
                       ("t", "DOUBLE", "float8")], rows)

    # duckdb-3d refuses polygons that admit no tiling; such rows are reported, not compared.
    duck_sql = """
        SELECT k, TRY(ST_3DDistance(ga, gb)), TRY(ST_3DDWithin(ga, gb, t)), TRY(ST_3DIntersects(ga, gb)),
               TRY(ST_3DMaxDistance(ga, gb)), TRY(ST_3DLength(ST_3DShortestLine(ga, gb))),
               TRY(ST_3DDistance(ST_3DClosestPoint(ga, gb), gb)), TRY(ST_3DDistance(ST_3DClosestPoint(ga, gb), ga))
        FROM (SELECT k, t, TRY(ST_Geom3DFromWKB(a)) AS ga, TRY(ST_Geom3DFromWKB(b)) AS gb FROM pairs)"""
    pg_sql = """
        SELECT k, ST_3DDistance(ga, gb), ST_3DDWithin(ga, gb, t), ST_3DIntersects(ga, gb), ST_3DMaxDistance(ga, gb)
        FROM (SELECT k, t, ST_GeomFromWKB(a) AS ga, ST_GeomFromWKB(b) AS gb FROM pairs) s"""
    ours = eng.duck_rows(duck_sql)
    theirs = eng.pg_rows(pg_sql)
    refused = {k for k, v in ours.items() if v[0] is None}

    classes = {k: _row_class(m[0], m[1]) for k, m in meta.items()}
    oracle_faces = {}

    def faces_of(it):
        if it.key not in oracle_faces:
            oracle_faces[it.key] = [OracleFace(f) for f in it.faces]
        return oracle_faces[it.key]

    reference = {}
    for k, (a, b, p, t) in meta.items():
        if k in refused:
            continue
        cls, bound = classes[k]
        if cls in ("holes", "non-planar"):
            d = oracle_point_distance(p, faces_of(a)) if p is not None else oracle_distance(faces_of(a), faces_of(b))
            pb = [p] if p is not None else [v for face in b.faces for ring in face for v in ring]
            reference[k] = (d, d <= t + 1e-9, d <= 1e-9, oracle_max_distance(a.faces, pb))
        else:
            reference[k] = theirs[k]

    out = []
    tol = args.distance_tol
    pg_beside = {k: theirs[k] for k in reference if classes[k][0] == "non-planar"}
    for cls, oracle, refs, info in (
            ("exact", "PostGIS", reference, False),
            ("holes", "independent oracle (planar polygons with holes)", reference, False),
            ("non-planar", "independent oracle, within twice the faces' deviation from their planes", reference, False),
            ("non-planar", "PostGIS, for information", pg_beside, True)):
        keys = [k for k in refs if classes[k][0] == cls]
        if not keys:
            continue
        for i, name in enumerate(["ST_3DDistance", "ST_3DDWithin", "ST_3DIntersects", "ST_3DMaxDistance"]):
            o = Outcome(f"{name} [{cls}]", oracle=oracle, informational=info)
            for k in keys:
                a, b, p, t = meta[k]
                bound = classes[k][1]
                mine, ref = ours[k][i], refs[k][i]
                o.compared += 1
                if isinstance(ref, float):
                    o.max_abs = max(o.max_abs, abs(mine - ref))
                    ok = abs(mine - ref) <= tol + bound + 1e-9 * abs(ref) if name != "ST_3DMaxDistance" \
                        else abs(mine - ref) <= tol + 1e-9 * abs(ref)
                else:
                    # A predicate may differ only where the reference distance is
                    # within the row's tolerance of the threshold.
                    d = refs[k][0]
                    edge = t if name == "ST_3DDWithin" else 0.0
                    ok = mine == ref or abs(d - edge) <= tol + bound
                if ok:
                    o.agree += 1
                else:
                    o.disagreements.append((k, (mine,), (ref,)))
            out.append(o)

    # Closest point and shortest line against their definitions, on every row.
    for i, name, check in ((4, "ST_3DShortestLine length is the distance", lambda v: v[4] - v[0]),
                           (5, "ST_3DClosestPoint is at the distance from the second", lambda v: v[5] - v[0]),
                           (6, "ST_3DClosestPoint lies on the first", lambda v: v[6])):
        o = Outcome(name, oracle="definition")
        for k, v in ours.items():
            if k in refused:
                continue
            o.compared += 1
            diff = abs(check(v))
            o.max_abs = max(o.max_abs, diff)
            if diff <= tol:
                o.agree += 1
            else:
                o.disagreements.append((k, (v[i],), (v[0] if i != 6 else 0.0,)))
        out.append(o)

    counts = {c: sum(1 for k in reference if classes[k][0] == c) for c in ("exact", "holes", "non-planar")}
    out[0].note = (f"{len(pairs)} object pairs with boxes within {args.within} m and {len(face_pairs)} face pairs "
                   f"(thresholds 0, 0.5, 2), {len(rows) - 3 * (len(pairs) + len(face_pairs))} face/point pairs "
                   f"({len([it for it in probe if it.holes])} holed faces probed at their holes' centres); rows per "
                   f"class: {counts}; {len(refused)} refused by duckdb-3d (a polygon admits no tiling); "
                   f"tolerance {tol} m")
    return out


def _ear_clip(face):
    """Triangles tiling a face without holes, each wound as the face is, by ear
    clipping in the projection that drops the Newell normal's largest axis; None
    for a face with holes or one ear clipping cannot tile. Independent of
    duckdb-3d's triangulator; on a non-planar face the two may pick different
    diagonals."""
    if len(face) != 1:
        return None
    ring = [v for i, v in enumerate(face[0]) if v != face[0][i - 1]]
    if len(ring) < 3:
        return None
    n = _newell(ring)
    drop = max(range(3), key=lambda i: abs(n[i]))
    u, w = [i for i in range(3) if i != drop]
    pts = [(v[u], v[w]) for v in ring]
    area = sum(pts[i][0] * pts[(i + 1) % len(pts)][1] - pts[(i + 1) % len(pts)][0] * pts[i][1]
               for i in range(len(pts)))
    order = list(range(len(ring))) if area > 0 else list(range(len(ring)))[::-1]

    def cross(o, a, b):
        return (a[0] - o[0]) * (b[1] - o[1]) - (a[1] - o[1]) * (b[0] - o[0])

    tris = []
    while len(order) > 3:
        for k in range(len(order)):
            i0, i1, i2 = order[k - 1], order[k], order[(k + 1) % len(order)]
            a, b, c = pts[i0], pts[i1], pts[i2]
            if cross(a, b, c) <= 0:
                continue
            if any(cross(a, b, pts[j]) >= 0 and cross(b, c, pts[j]) >= 0 and cross(c, a, pts[j]) >= 0
                   for j in order if j not in (i0, i1, i2) and pts[j] not in (a, b, c)):
                continue
            tris.append((i0, i1, i2))
            del order[k]
            break
        else:
            return None
    tris.append(tuple(order))
    if area < 0:
        tris = [t[::-1] for t in tris]
    return [[ring[i] for i in t] for t in tris]


def _triangle_surface(faces) -> bytes | None:
    """A PolyhedralSurface Z of the faces' ear-clipped triangles, for SFCGAL."""
    tris = []
    for face in faces:
        t = _ear_clip(face)
        if t is None:
            return None
        tris.extend(t)
    out = struct.pack("<BII", 1, 1015, len(tris))
    for t in tris:
        out += struct.pack("<BIII", 1, 1003, 1, 4)
        for v in t + [t[0]]:
            out += struct.pack("<ddd", *v)
    return out


def check_solids(eng: Engines, items: list[Item], args) -> list[Outcome]:
    """The SOLID_3D overloads of the distance family, which measure the volume:
    points against solids and solid pairs, against SFCGAL's exact 3D intersection
    of the solid (ST_MakeSolid) for containment and PostGIS (or the independent
    oracle, as in the distance check) for the distance to the surface."""
    rng = random.Random(args.seed + 1)
    solids = [it for it in items if it.role == "solid" and len(it.shells) == 1]
    rng.shuffle(solids)
    probe = solids[: args.points]
    tri = {it.key: _triangle_surface(it.faces) for it in solids}
    rows, meta = [], {}
    for it in probe:
        x0, y0, z0, x1, y1, z1 = it.bbox
        pts = [(rng.uniform(x0, x1), rng.uniform(y0, y1), rng.uniform(z0, z1)) for _ in range(6)]
        # Points straight below and above a vertex, and level with one: the
        # vertical ray of the containment test passes exactly through vertices
        # and along edges.
        verts = [v for face in it.faces for ring in face for v in ring]
        for v in rng.sample(verts, min(3, len(verts))):
            pts.append((v[0], v[1], rng.uniform(z0, z1)))
            pts.append((v[0], rng.uniform(y0, y1), v[2]))
        for j, p in enumerate(pts):
            k = f"{it.key}|pt{j}"
            rows.append((k, it.wkb, json.dumps({"type": "Solid", "shells": [it.shells]}), point_wkb(*p), it.wkb,
                         tri[it.key], point_wkb(*p)))
            meta[k] = (it, None, p)
    pairs = near_pairs(solids, 0.5, args.pairs // 4, rng)
    for a, b in pairs:
        k = f"{a.key}|{b.key}"
        rows.append((k, a.wkb, json.dumps({"type": "Solid", "shells": [a.shells]}), b.wkb,
                     json.dumps({"type": "Solid", "shells": [b.shells]}), tri[a.key], tri[b.key]))
        meta[k] = (a, b, None)
    eng.load("solid_pairs", [("k", "VARCHAR", "text"), ("a", "BLOB", "bytea"), ("sa", "VARCHAR", "text"),
                             ("b", "BLOB", "bytea"), ("sb", "VARCHAR", "text"), ("ta", "BLOB", "bytea"),
                             ("tb", "BLOB", "bytea")], rows)
    duck_sql = """
        SELECT k, (ST_3DValidationReport(s)).is_valid,
               TRY(ST_3DIntersects(s, g)), TRY(ST_3DDistance(s, g)), TRY(ST_3DDWithin(s, g, 0.5))
        FROM (SELECT k, ST_3DFromWKB(a, sa) AS s,
                     CASE WHEN k LIKE '%|pt%' THEN ST_Geom3DFromWKB(b) END AS g FROM solid_pairs)
        WHERE g IS NOT NULL
        UNION ALL
        SELECT k, (ST_3DValidationReport(s)).is_valid AND (ST_3DValidationReport(t)).is_valid,
               TRY(ST_3DIntersects(s, t)), TRY(ST_3DDistance(s, t)), TRY(ST_3DDWithin(s, t, 0.5))
        FROM (SELECT k, ST_3DFromWKB(a, sa) AS s, ST_3DFromWKB(b, sb) AS t FROM solid_pairs WHERE k NOT LIKE '%|pt%')"""
    # SFCGAL accepts only exactly planar faces, which real roofs rarely are, so the
    # solid it tests is made of each face's triangles, ear-clipped here (ta, tb).
    # Where a face is not planar that surface differs from duckdb-3d's own
    # triangulation by up to the face's deviation, which the comparison allows
    # for. A solid SFCGAL refuses, or one with a face ear clipping cannot tile,
    # gives NULL and the row is skipped.
    with eng.pg.cursor() as cur:
        cur.execute("""
            CREATE OR REPLACE FUNCTION oracle_intersects(s bytea, g bytea, g_solid boolean) RETURNS boolean AS $$
            BEGIN
              IF s IS NULL OR g IS NULL THEN RETURN NULL; END IF;
              RETURN NOT ST_IsEmpty(ST_3DIntersection(ST_MakeSolid(ST_GeomFromWKB(s)),
                CASE WHEN g_solid THEN ST_MakeSolid(ST_GeomFromWKB(g)) ELSE ST_GeomFromWKB(g) END));
            EXCEPTION WHEN others THEN RETURN NULL;
            END $$ LANGUAGE plpgsql""")
    # One statement per row with a time limit: SFCGAL's 3D intersection of two
    # solids builds Nef polyhedra and can take minutes on a detailed pair. A row
    # it does not finish is skipped like a refused one. SFCGAL does not check for
    # interrupts, so the limit takes effect only when its call returns.
    ours = eng.duck_rows(duck_sql)
    theirs = {}
    with eng.pg.cursor() as cur:
        cur.execute(f"SET statement_timeout = '{int(args.sfcgal_timeout * 1000)}'")
        for n, k in enumerate(meta):
            try:
                cur.execute("""
                    SELECT oracle_intersects(ta, tb, k NOT LIKE '%%|pt%%'),
                           ST_3DDistance(ST_GeomFromWKB(a), ST_GeomFromWKB(b))
                    FROM solid_pairs WHERE k = %s""", (k,))
                theirs[k] = cur.fetchone()
            except psycopg2.errors.QueryCanceled:
                theirs[k] = (None, None)
            if n % 500 == 0:
                print(f"solids: {n}/{len(meta)} rows against SFCGAL", file=sys.stderr)
        cur.execute("RESET statement_timeout")
    refused = {k for k, v in theirs.items() if v[0] is None}
    tol = args.distance_tol
    invalid = {k for k, v in ours.items() if not v[0]}
    raised_ok = sum(1 for k in invalid if ours[k][1] is None and ours[k][2] is None)
    out = []
    for kind, label in (("point", "points against solids"), ("pair", "solid pairs")):
        o_int = Outcome(f"ST_3DIntersects(SOLID_3D, ...) [{label}]",
                        oracle="SFCGAL ST_3DIntersection of ST_MakeSolid")
        o_dist = Outcome(f"ST_3DDistance(SOLID_3D, ...) [{label}]",
                         oracle="0 when SFCGAL intersects, else the surface distance (PostGIS or independent oracle)")
        o_dw = Outcome(f"ST_3DDWithin(SOLID_3D, ..., 0.5) [{label}]", oracle="the reference distance <= 0.5")
        for k, (a, b, p) in meta.items():
            if (p is not None) != (kind == "point") or k in invalid:
                continue
            if k in refused:
                o_int.skipped["SFCGAL refuses the solid or times out"] = o_int.skipped.get("SFCGAL refuses the solid or times out", 0) + 1
                continue
            cls, bound = _row_class(a, b)
            inside, surface = theirs[k]
            if cls != "exact":
                ref_faces = [OracleFace(f) for f in a.faces]
                if p is not None:
                    surface = oracle_point_distance(p, ref_faces)
                else:
                    surface = oracle_distance(ref_faces, [OracleFace(f) for f in b.faces])
            ref = 0.0 if inside else surface
            mine_int, mine_d, mine_dw = ours[k][1:]
            # Where the reference surface distance is within the row's tolerance
            # of 0, the two triangulations of a non-planar face may decide
            # containment differently; there a predicate may differ.
            near = surface <= tol + bound
            for o, mine, theirs_v in ((o_int, mine_int, inside), (o_dw, mine_dw, ref <= 0.5)):
                o.compared += 1
                if mine == theirs_v or near or (o is o_dw and abs(ref - 0.5) <= tol + bound):
                    o.agree += 1
                else:
                    o.disagreements.append((k, (mine,), (theirs_v,)))
            if surface is None:
                o_dist.skipped["SFCGAL timed out"] = o_dist.skipped.get("SFCGAL timed out", 0) + 1
                continue
            o_dist.compared += 1
            o_dist.max_abs = max(o_dist.max_abs, abs(mine_d - ref))
            if abs(mine_d - ref) <= tol + bound + 1e-9 * abs(ref) or (near and mine_d <= tol + bound):
                o_dist.agree += 1
            else:
                o_dist.disagreements.append((k, (mine_d,), (ref,)))
        out.extend([o_int, o_dist, o_dw])
    inside_points = sum(1 for k, (a, b, p) in meta.items()
                        if p is not None and k not in invalid and k not in refused and theirs[k][0])
    out[0].note = (f"{len(probe)} single-shell solids, {sum(1 for m in meta.values() if m[2] is not None)} points "
                   f"(6 random in the box, 6 on vertical and horizontal lines through vertices), {inside_points} "
                   f"of them inside or on the solid per SFCGAL; {len(pairs)} solid pairs with boxes within 0.5 m; "
                   f"{len(invalid)} rows with an invalid solid skipped, {raised_ok} of them confirmed to raise "
                   f"(TRY gives NULL)")
    return out


def _length(v):
    return math.sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2])


def _cross(a, b):
    return (a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0])


def oracle_area(faces) -> float:
    """Area of planar faces, holes subtracted: half the length of each ring's
    Newell vector. On a non-planar face this is the projected (vector) area, which
    differs from any triangulation's area by second order in the face's deviation."""
    return sum(_length(_newell(face[0])) / 2 - sum(_length(_newell(h)) / 2 for h in face[1:]) for face in faces)


def oracle_centroid(item: Item):
    """(centroid, volume) of a solid from the ear-clipped triangles of its faces:
    the signed tetrahedra about the box's lower corner, summed over every shell
    (inner shells face inward, so they subtract). None when a face has a hole or
    ear clipping cannot tile it."""
    o = item.bbox[:3]
    vol = mx = my = mz = 0.0
    for face in item.faces:
        tris = _ear_clip(face)
        if tris is None:
            return None
        for a, b, c in tris:
            a, b, c = (tuple(p[k] - o[k] for k in range(3)) for p in (a, b, c))
            v = (a[0] * (b[1] * c[2] - b[2] * c[1]) - a[1] * (b[0] * c[2] - b[2] * c[0])
                 + a[2] * (b[0] * c[1] - b[1] * c[0])) / 6.0
            vol += v
            mx += v * (a[0] + b[0] + c[0]) / 4.0
            my += v * (a[1] + b[1] + c[1]) / 4.0
            mz += v * (a[2] + b[2] + c[2]) / 4.0
    if vol == 0.0:
        return None
    return (o[0] + mx / vol, o[1] + my / vol, o[2] + mz / vol), vol


def centroid_slack(item: Item, volume: float) -> float:
    """How far a solid's centroid can move between two triangulations of its
    non-planar faces. Two triangulations of a face lie within a slab of twice its
    deviation, so they enclose volumes differing by at most 2 * deviation * area;
    that volume sits within the box's diagonal of the centroid."""
    swept = sum(2 * face_deviation(f) * oracle_area([f]) for f in item.faces)
    return swept * math.dist(item.bbox[:3], item.bbox[3:]) / abs(volume)


def sfcgal_face(face) -> bytes:
    """A face as Polygon Z WKB for SFCGAL, its exterior ring starting at a vertex
    whose two successors are not collinear with it: SFCGAL takes a polygon's
    plane from its first three vertices and gives area 0 when they are collinear."""
    ring = face[0]
    n = len(ring)
    start = 0
    for i in range(n):
        a, b, c = ring[i], ring[(i + 1) % n], ring[(i + 2) % n]
        if _length(_cross(_sub(b, a), _sub(c, a))) > 1e-9 * max(1.0, _length(_sub(b, a)) * _length(_sub(c, a))):
            start = i
            break
    rings = [ring[start:] + ring[:start]] + face[1:]
    out = bytearray(struct.pack("<BII", 1, 1003, len(rings)))
    for r in rings:
        out += struct.pack("<I", len(r) + 1)
        for v in r + r[:1]:
            out += struct.pack("<3d", *v)
    return bytes(out)


def _perimeter(faces) -> float:
    return sum(math.dist(a, b) for face in faces for ring in face for a, b in zip(ring, ring[1:] + ring[:1]))


def oracle_boundary(faces) -> float:
    """Length of a surface's boundary: the edges exactly one face uses."""
    uses = {}
    for face in faces:
        for ring in face:
            for a, b in zip(ring, ring[1:] + ring[:1]):
                if a != b:
                    e = (min(a, b), max(a, b))
                    uses[e] = uses.get(e, 0) + 1
    return sum(math.dist(*e) for e, n in uses.items() if n == 1)


def check_measures(eng: Engines, items: list[Item], args) -> list[Outcome]:
    """ST_3DArea, ST_3DPerimeter and ST_3DNumGeometries on GEOM_3D against
    PostGIS (SFCGAL for the area) and an independent area; ST_3DCentroid on
    SOLID_3D against an independent centroid, since PostGIS has no 3D centroid."""
    rows = [(it.key, it.wkb, json.dumps({"type": "Solid", "shells": [it.shells]}) if it.role == "solid" else None,
             sfcgal_face(it.faces[0]) if it.role == "face" else None) for it in items]
    eng.load("measure_items", [("k", "VARCHAR", "text"), ("w", "BLOB", "bytea"), ("s", "VARCHAR", "text"),
                               ("fw", "BLOB", "bytea")], rows)
    ours = eng.duck_rows("""
        SELECT k, ST_3DArea(g), ST_3DPerimeter(g), ST_3DNumGeometries(g),
               ST_3DX(c), ST_3DY(c), ST_3DZ(c)
        FROM (SELECT k, TRY(ST_Geom3DFromWKB(w)) AS g,
                     CASE WHEN s IS NOT NULL THEN TRY(ST_3DCentroid(ST_3DFromWKB(w, s))) END AS c
              FROM measure_items)""")
    with eng.pg.cursor() as cur:
        cur.execute("""
            CREATE OR REPLACE FUNCTION oracle_area(g geometry) RETURNS double precision AS $$
            BEGIN RETURN ST_3DArea(g); EXCEPTION WHEN others THEN RETURN NULL; END $$ LANGUAGE plpgsql""")
    # SFCGAL's area only for single faces: on some whole buildings it runs for
    # minutes and heeds no time limit. Surfaces and solids have the independent
    # area instead.
    theirs = eng.pg_rows("""
        SELECT k, CASE WHEN fw IS NOT NULL THEN oracle_area(ST_GeomFromWKB(fw)) END, ST_3DPerimeter(g),
               ST_NumGeometries(g)
        FROM (SELECT k, fw, ST_GeomFromWKB(w) AS g FROM measure_items) t""")
    tol = args.distance_tol
    by_key = {it.key: it for it in items}
    o_sfcgal = Outcome("ST_3DArea(GEOM_3D)", oracle="SFCGAL ST_3DArea")
    o_newell = Outcome("ST_3DArea(GEOM_3D)", oracle="independent: Newell vector area per ring")
    o_perim = Outcome("ST_3DPerimeter(GEOM_3D) [polygons]", oracle="PostGIS ST_3DPerimeter")
    o_bound = Outcome("ST_3DPerimeter(GEOM_3D) [surfaces]", oracle="independent: edges one face uses")
    o_ngeom = Outcome("ST_3DNumGeometries(GEOM_3D)", oracle="PostGIS ST_NumGeometries")
    o_cent = Outcome("ST_3DCentroid(SOLID_3D)", oracle="independent: signed tetrahedra over ear-clipped faces")
    planar_max = 0.0
    refused = 0
    strict = 0
    for k, (area, perim, ngeom, cx, cy, cz) in ours.items():
        it = by_key[k]
        p_area, p_perim, p_ngeom = theirs[k]
        if area is None:
            # ST_Geom3DFromWKB refuses a polygon that admits no triangulation.
            refused += 1
            continue
        # A non-planar face has no single area: triangulations differ by second
        # order in the deviation, which perimeter * deviation bounds generously.
        bound = tol * max(1.0, area) + _perimeter(it.faces) * it.deviation
        for o, ref in ((o_sfcgal, p_area), (o_newell, oracle_area(it.faces))):
            if ref is None:
                why = "not a single face, or SFCGAL refuses it"
                o.skipped[why] = o.skipped.get(why, 0) + 1
                continue
            o.compared += 1
            o.max_abs = max(o.max_abs, abs(area - ref))
            if it.deviation == 0.0:
                planar_max = max(planar_max, abs(area - ref) / max(1.0, ref))
            if abs(area - ref) <= bound:
                o.agree += 1
            else:
                o.disagreements.append((k, (area,), (ref,)))
        # PostGIS sums every ring of a PolyhedralSurface; SQL/MM, and duckdb-3d,
        # take its boundary.
        o, ref = (o_perim, p_perim) if it.role == "face" else (o_bound, oracle_boundary(it.faces))
        o.compared += 1
        o.max_abs = max(o.max_abs, abs(perim - ref))
        if abs(perim - ref) <= 1e-9 * max(1.0, ref):
            o.agree += 1
        else:
            o.disagreements.append((k, (perim,), (ref,)))
        o_ngeom.compared += 1
        if ngeom == p_ngeom:
            o_ngeom.agree += 1
        else:
            o_ngeom.disagreements.append((k, (ngeom,), (p_ngeom,)))
        if it.role != "solid":
            continue
        if cx is None:
            o_cent.skipped["the solid is not closed (raises)"] = o_cent.skipped.get("the solid is not closed (raises)", 0) + 1
            continue
        got = oracle_centroid(it)
        if got is None:
            o_cent.skipped["a face ear clipping cannot tile"] = o_cent.skipped.get("a face ear clipping cannot tile", 0) + 1
            continue
        ref, volume = got
        d = math.dist((cx, cy, cz), ref)
        o_cent.compared += 1
        o_cent.max_abs = max(o_cent.max_abs, d)
        slack = centroid_slack(it, volume)
        strict += slack <= tol
        if d <= tol + slack:
            o_cent.agree += 1
        else:
            o_cent.disagreements.append((k, (cx, cy, cz), ref))
    kinds = {r: sum(1 for it in items if it.role == r) for r in ("solid", "surface", "face")}
    o_sfcgal.note = (f"{kinds['solid']} solids and {kinds['surface']} surfaces as PolyhedralSurface Z, "
                     f"{kinds['face']} single faces as Polygon Z; tolerance {tol:g} relative plus perimeter times "
                     f"the largest face deviation; on exactly planar rows the largest relative difference over both "
                     f"references is {planar_max:.3g}")
    o_cent.note = ("tolerance: distance-tol plus how far the centroid can move between two triangulations of "
                   f"the non-planar faces; for {strict} solids that slack is below distance-tol")
    o_newell.note = f"{refused} geometries skipped: ST_Geom3DFromWKB refuses a polygon with no triangulation"
    return [o_sfcgal, o_newell, o_perim, o_bound, o_ngeom, o_cent]


def _tally(o: Outcome, key, ours, theirs, equal) -> None:
    o.compared += 1
    if equal:
        o.agree += 1
    else:
        o.disagreements.append((key, ours, theirs))


def oracle_slope_azimuth(face):
    """(slope, azimuth) in degrees of a face from its Newell normal, computed
    here; azimuth None for a horizontal face, both None without area."""
    n = [0.0, 0.0, 0.0]
    for ring in face:
        v = _newell(ring)
        n = [n[k] + v[k] for k in range(3)]
    length = _length(n)
    if length == 0.0:
        return None, None
    n = [c / length for c in n]
    slope = math.degrees(math.atan2(math.hypot(n[0], n[1]), abs(n[2])))
    if n[0] == 0.0 and n[1] == 0.0:
        return slope, None
    return slope, math.degrees(math.atan2(n[0], n[1])) % 360.0


def check_elements(eng: Engines, items: list[Item], args) -> list[Outcome]:
    """Bounds, ST_3DExtent, parts, faces, shells and WKT against PostGIS on the
    same WKB; per-face slope and azimuth against a Newell normal computed here."""
    rows = [(it.key, it.dataset, it.wkb,
             json.dumps({"type": "Solid", "shells": [it.shells]}) if it.role == "solid" else None) for it in items]
    eng.load("element_items", [("k", "VARCHAR", "text"), ("d", "VARCHAR", "text"), ("w", "BLOB", "bytea"),
                               ("s", "VARCHAR", "text")], rows)
    theirs = eng.pg_rows("""
        SELECT k, ST_XMin(g), ST_YMin(g), ST_ZMin(g), ST_XMax(g), ST_YMax(g), ST_ZMax(g),
               ARRAY(SELECT ST_AsBinary(ST_GeometryN(g, i), 'NDR') FROM generate_series(1, ST_NumGeometries(g)) i),
               ST_AsText(g, 20), ST_AsEWKT(ST_SetSRID(g, 7415), 20), ST_AsEWKB(ST_SetSRID(g, 28992), 'NDR'),
               ST_AsBinary(ST_GeomFromText(ST_AsText(g, 20)), 'NDR'),
               ARRAY(SELECT ST_AsBinary(ST_RemoveRepeatedPoints(ST_GeometryN(g, i)), 'NDR')
                     FROM generate_series(1, ST_NumGeometries(g)) i)
        FROM (SELECT k, ST_GeomFromWKB(w) AS g FROM element_items) t""")
    eng.load("element_text", [("k", "VARCHAR", "text"), ("wkt", "VARCHAR", "text"), ("ewkt", "VARCHAR", "text"),
                              ("ewkb", "BLOB", "bytea"), ("wkt_wkb", "BLOB", "bytea")],
             [(k, v[7], v[8], bytes(v[9]), bytes(v[10])) for k, v in theirs.items()])
    ours = eng.duck_rows("""
        SELECT i.k, ST_3DXMin(g), ST_3DYMin(g), ST_3DZMin(g), ST_3DXMax(g), ST_3DYMax(g), ST_3DZMax(g),
               list_transform(range(1, ST_3DNumGeometries(g) + 1), lambda n: ST_3DAsBinary(ST_3DGeometryN(g, n))),
               ST_3DGeomFromText(t.wkt)::BLOB = ST_Geom3DFromWKB(t.wkt_wkb)::BLOB,
               ST_3DCRS(ST_3DGeomFromText(t.ewkt)), ST_3DSRID(ST_Geom3DFromWKB(t.ewkb)),
               CASE WHEN s IS NOT NULL THEN list_transform(ST_3DFaces(ST_3DFromWKB(w, s)), lambda f: ST_3DAsBinary(f.polygon)) END,
               CASE WHEN s IS NOT NULL THEN ST_3DNumSolids(ST_3DFromWKB(w, s)) END,
               CASE WHEN s IS NOT NULL THEN ST_3DAsBinary(ST_3DExteriorShell(ST_3DFromWKB(w, s))) END,
               ST_3DFaceSlopes(g), ST_3DFaceAzimuths(g)
        FROM (SELECT k, w, s, TRY(ST_Geom3DFromWKB(w)) AS g FROM element_items) i JOIN element_text t ON i.k = t.k
        WHERE g IS NOT NULL""")
    by_key = {it.key: it for it in items}
    o_box = Outcome("ST_3DXMin .. ST_3DZMax", oracle="PostGIS ST_XMin .. ST_ZMax")
    o_ext = Outcome("ST_3DExtent", oracle="PostGIS ST_3DExtent")
    o_parts = Outcome("ST_3DGeometryN(GEOM_3D)", oracle="PostGIS ST_AsBinary(ST_GeometryN), byte for byte")
    o_wkt = Outcome("ST_3DGeomFromText", oracle="PostGIS ST_GeomFromText of the same text, as WKB")
    o_srid = Outcome("CRS from EWKT and EWKB", oracle="PostGIS ST_AsEWKT / ST_AsEWKB of ST_SetSRID")
    o_faces = Outcome("ST_3DFaces(SOLID_3D)",
                      oracle="PostGIS ST_RemoveRepeatedPoints of each ST_GeometryN of the solid's surface, as WKB")
    o_shell = Outcome("ST_3DExteriorShell", oracle="the PolyhedralSurface of those faces, for one-shell solids")
    o_slope = Outcome("ST_3DFaceSlopes, ST_3DFaceAzimuths", oracle="independent: Newell normal per face")
    for k, (x0, y0, z0, x1, y1, z1, parts, wkt_same, ewkt_crs, ewkb_srid, faces, nsolids, shell, slopes,
            azimuths) in ours.items():
        it = by_key[k]
        p = theirs[k]
        _tally(o_box, k, (x0, y0, z0, x1, y1, z1), tuple(p[:6]), (x0, y0, z0, x1, y1, z1) == tuple(p[:6]))
        pg_parts = [bytes(b) for b in p[6]]
        _tally(o_parts, k, len(parts), len(pg_parts), [bytes(b) for b in parts] == pg_parts)
        _tally(o_wkt, k, wkt_same, True, wkt_same is True)
        _tally(o_srid, k, (ewkt_crs, ewkb_srid), ("EPSG:7415", 28992), (ewkt_crs, ewkb_srid) == ("EPSG:7415", 28992))
        if it.role == "solid":
            # A solid's import drops a vertex repeated consecutively (DESIGN_DOC §2.1).
            # PostGIS keeps four points in a ring however many repeat, so a ring
            # with fewer than three distinct vertices is left out of the comparison.
            pg_faces = [bytes(b) for b in p[11]]
            short = {f for f, face in enumerate(it.faces) if any(len(set(ring)) < 3 for ring in face)}
            if short:
                why = "a ring with fewer than three distinct vertices"
                o_faces.skipped[why] = o_faces.skipped.get(why, 0) + 1
                o_shell.skipped[why] = o_shell.skipped.get(why, 0) + 1
            else:
                _tally(o_faces, k, len(faces), len(pg_faces), [bytes(b) for b in faces] == pg_faces)
                if nsolids == 1 and len(it.shells) == 1:
                    expected = struct.pack("<BII", 1, 1015, len(pg_faces)) + b"".join(pg_faces)
                    _tally(o_shell, k, len(shell), len(expected), bytes(shell) == expected)
        for f, face in enumerate(it.faces):
            ref_slope, ref_az = oracle_slope_azimuth(face)
            got_slope, got_az = slopes[f], azimuths[f]
            same = ((ref_slope is None) == (got_slope is None) and (ref_az is None) == (got_az is None)
                    and (ref_slope is None or abs(got_slope - ref_slope) <= 1e-9)
                    and (ref_az is None or min(abs(got_az - ref_az), 360 - abs(got_az - ref_az)) <= 1e-9))
            if ref_slope is not None and got_slope is not None:
                o_slope.max_abs = max(o_slope.max_abs, abs(got_slope - ref_slope))
            _tally(o_slope, f"{k}/{f}", (got_slope, got_az), (ref_slope, ref_az), same)
    ext_ours = eng.duck_rows("""
        SELECT d, e.min_x, e.min_y, e.min_z, e.max_x, e.max_y, e.max_z
        FROM (SELECT d, ST_3DExtent(TRY(ST_Geom3DFromWKB(w))) AS e FROM element_items GROUP BY d)""")
    ext_theirs = eng.pg_rows("""
        SELECT d, ST_XMin(e), ST_YMin(e), ST_ZMin(e), ST_XMax(e), ST_YMax(e), ST_ZMax(e)
        FROM (SELECT d, ST_3DExtent(ST_GeomFromWKB(w)) AS e FROM element_items GROUP BY d) t""")
    for d, box in ext_ours.items():
        _tally(o_ext, d, box, ext_theirs[d], tuple(box) == tuple(ext_theirs[d]))
    o_ext.note = "per dataset; rows ST_Geom3DFromWKB refuses are left out of ours and kept in PostGIS's"
    o_slope.note = (f"slope to 1e-9 degrees, azimuth to 1e-9 degrees around the circle; largest slope "
                    f"difference {o_slope.max_abs:.3g}")
    return [o_box, o_ext, o_parts, o_wkt, o_srid, o_faces, o_shell, o_slope]


CHECKS = {"distance": check_distance, "solids": check_solids, "measures": check_measures,
          "elements": check_elements}


# --------------------------------------------------------------------------
# Report
# --------------------------------------------------------------------------

def write_report(path: Path, outcomes: list[Outcome], eng: Engines, args, counts: dict):
    lines = ["# duckdb-3d against PostGIS on real data", ""]
    lines.append(f"- Reference: `{eng.pg_version}`")
    lines.append(f"- duckdb-3d: `{args.extension}`")
    lines.append(f"- Datasets: " + ", ".join(f"{d} ({n} geometries)" for d, n in counts.items()))
    lines.append(f"- Seed {args.seed}")
    lines.append("")
    lines.append("| Check | Reference | Compared | Agree | Disagree | Max abs diff | Skipped |")
    lines.append("|---|---|---|---|---|---|---|")
    for o in outcomes:
        skipped = "; ".join(f"{v} {k}" for k, v in o.skipped.items()) or "0"
        lines.append(f"| {o.check} | {o.oracle} | {o.compared} | {o.agree} | {len(o.disagreements)} | {o.max_abs:.3g} "
                     f"| {skipped} |")
    lines.append("")
    for o in outcomes:
        if o.note:
            lines.append(f"- **{o.check}**: {o.note}")
    for o in outcomes:
        if not o.disagreements:
            continue
        lines.append("")
        lines.append(f"## {o.check}: {len(o.disagreements)} disagreements (first 25)")
        lines.append("")
        lines.append("| Key | duckdb-3d | reference |")
        lines.append("|---|---|---|")
        for key, a, b in o.disagreements[:25]:
            lines.append(f"| `{key}` | {a} | {b} |")
    path.write_text("\n".join(lines) + "\n")


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--datasets", default=",".join(DATASETS))
    ap.add_argument("--checks", default=",".join(CHECKS))
    ap.add_argument("--workdir", type=Path, default=REPO / "build" / "live_diff")
    ap.add_argument("--extension", type=Path, default=EXTENSION)
    ap.add_argument("--dsn", default="dbname=oracle")
    ap.add_argument("--report", type=Path, default=None)
    ap.add_argument("--seed", type=int, default=20261015)
    ap.add_argument("--pairs", type=int, default=3000, help="object pairs (and face pairs) per run")
    ap.add_argument("--points", type=int, default=1500, help="faces probed with random points")
    ap.add_argument("--within", type=float, default=5.0, help="box gap for an object pair, in metres")
    ap.add_argument("--faces", type=int, default=20000, help="faces kept per dataset")
    ap.add_argument("--distance-tol", type=float, default=1e-6)
    ap.add_argument("--sfcgal-timeout", type=float, default=20.0, help="seconds per SFCGAL row (solids check)")
    args = ap.parse_args()

    args.workdir.mkdir(parents=True, exist_ok=True)
    items: list[Item] = []
    counts = {}
    for name in args.datasets.split(","):
        got = read_dataset(name, fetch(name, args.workdir), args.faces)
        counts[name] = len(got)
        items.extend(got)
    eng = Engines(args.extension, args.dsn)
    outcomes = []
    for name in args.checks.split(","):
        outcomes.extend(CHECKS[name](eng, items, args))
    report = args.report or (args.workdir / "report.md")
    write_report(report, outcomes, eng, args, counts)
    print(report.read_text())
    if any(o.disagreements for o in outcomes if not o.informational):
        sys.exit(1)


if __name__ == "__main__":
    import psycopg2.extras  # noqa: F401  (execute_values)
    main()
