#include "kernel/geom_distance.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace duckdb_3d {

namespace {

struct Vec3 {
	double x, y, z;
};
Vec3 Sub(const Vertex3D &a, const Vertex3D &b) {
	return {a.x - b.x, a.y - b.y, a.z - b.z};
}
double Dot(const Vec3 &a, const Vec3 &b) {
	return a.x * b.x + a.y * b.y + a.z * b.z;
}
double Clamp01(double t) {
	return std::max(0.0, std::min(1.0, t));
}
Vec3 Cross(const Vec3 &a, const Vec3 &b) {
	return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

//! Möller–Trumbore segment/triangle intersection (segment [p,q]).
bool SegmentIntersectsTriangle(const Vertex3D &p, const Vertex3D &q, const Vertex3D &a, const Vertex3D &b,
                               const Vertex3D &c) {
	const double eps = kEpsAbsolute;
	Vec3 dir = Sub(q, p);
	Vec3 e1 = Sub(b, a);
	Vec3 e2 = Sub(c, a);
	Vec3 h = Cross(dir, e2);
	double det = Dot(e1, h);
	if (det > -eps && det < eps) {
		return false; // segment parallel to triangle plane
	}
	double inv = 1.0 / det;
	Vec3 s = Sub(p, a);
	double u = inv * Dot(s, h);
	if (u < 0.0 || u > 1.0) {
		return false;
	}
	Vec3 qv = Cross(s, e1);
	double v = inv * Dot(dir, qv);
	if (v < 0.0 || u + v > 1.0) {
		return false;
	}
	double t = inv * Dot(e2, qv);
	return t >= 0.0 && t <= 1.0;
}

} // namespace

double DistPointPoint(const Vertex3D &p, const Vertex3D &q) {
	Vec3 d = Sub(p, q);
	return std::sqrt(Dot(d, d));
}

// Forward declaration: defined below.
Vertex3D ClosestPointOnSegment(const Vertex3D &p, const Vertex3D &a, const Vertex3D &b);

double DistPointSegment(const Vertex3D &p, const Vertex3D &a, const Vertex3D &b) {
	return DistPointPoint(p, ClosestPointOnSegment(p, a, b));
}

//! Closest point on triangle (a,b,c) to point p.
Vertex3D ClosestPointOnTriangle(const Vertex3D &p, const Vertex3D &a, const Vertex3D &b, const Vertex3D &c) {
	// Ericson, Real-Time Collision Detection §5.1.5 (ClosestPtPointTriangle).
	Vec3 ab = Sub(b, a);
	Vec3 ac = Sub(c, a);
	Vec3 ap = Sub(p, a);
	double d1 = Dot(ab, ap);
	double d2 = Dot(ac, ap);
	auto at = [&](double u, double v, double w) {
		// Barycentric (u,v,w) → cartesian closest point on triangle.
		return Vertex3D {a.x * u + b.x * v + c.x * w, a.y * u + b.y * v + c.y * w, a.z * u + b.z * v + c.z * w};
	};
	if (d1 <= 0.0 && d2 <= 0.0) {
		return a; // vertex region a
	}
	Vec3 bp = Sub(p, b);
	double d3 = Dot(ab, bp);
	double d4 = Dot(ac, bp);
	if (d3 >= 0.0 && d4 <= d3) {
		return b; // vertex region b
	}
	double vc = d1 * d4 - d3 * d2;
	if (vc <= 0.0 && d1 >= 0.0 && d3 <= 0.0) {
		double v = d1 / (d1 - d3);
		return at(1.0 - v, v, 0.0); // edge ab
	}
	Vec3 cp = Sub(p, c);
	double d5 = Dot(ab, cp);
	double d6 = Dot(ac, cp);
	if (d6 >= 0.0 && d5 <= d6) {
		return c; // vertex region c
	}
	double vb = d5 * d2 - d1 * d6;
	if (vb <= 0.0 && d2 >= 0.0 && d6 <= 0.0) {
		double w = d2 / (d2 - d6);
		return at(1.0 - w, 0.0, w); // edge ac
	}
	double va = d3 * d6 - d5 * d4;
	if (va <= 0.0 && (d4 - d3) >= 0.0 && (d5 - d6) >= 0.0) {
		double w = (d4 - d3) / ((d4 - d3) + (d5 - d6));
		return at(0.0, 1.0 - w, w); // edge bc
	}
	// Interior: project onto the face plane via barycentric coords.
	double denom = 1.0 / (va + vb + vc);
	double v = vb * denom;
	double w = vc * denom;
	return at(1.0 - v - w, v, w);
}

double DistPointTriangle(const Vertex3D &p, const Vertex3D &a, const Vertex3D &b, const Vertex3D &c) {
	return DistPointPoint(p, ClosestPointOnTriangle(p, a, b, c));
}

//! Closest point on segment [a,b] to point p.
Vertex3D ClosestPointOnSegment(const Vertex3D &p, const Vertex3D &a, const Vertex3D &b) {
	Vec3 ab = Sub(b, a);
	Vec3 ap = Sub(p, a);
	double len2 = Dot(ab, ab);
	double t = (len2 > 0.0) ? Clamp01(Dot(ap, ab) / len2) : 0.0;
	return Vertex3D {a.x + ab.x * t, a.y + ab.y * t, a.z + ab.z * t};
}

//! Closest points between two segments [p1,q1] and [p2,q2].
//! Returns (c1 on segment 1, c2 on segment 2).
ClosestPointPair ClosestPointPairSegmentSegment(const Vertex3D &p1, const Vertex3D &q1, const Vertex3D &p2,
                                                const Vertex3D &q2) {
	// Ericson, Real-Time Collision Detection §5.1.9 (ClosestPtSegmentSegment).
	Vec3 d1 = Sub(q1, p1); // direction of segment 1
	Vec3 d2 = Sub(q2, p2); // direction of segment 2
	Vec3 r = Sub(p1, p2);
	double a = Dot(d1, d1);
	double e = Dot(d2, d2);
	double f = Dot(d2, r);

	double s, t;
	const double eps = kEpsAbsolute;
	if (a <= eps && e <= eps) {
		s = t = 0.0; // both segments are points
	} else if (a <= eps) {
		s = 0.0;
		t = Clamp01(f / e); // segment 1 is a point
	} else {
		double c = Dot(d1, r);
		if (e <= eps) {
			t = 0.0;
			s = Clamp01(-c / a); // segment 2 is a point
		} else {
			double b = Dot(d1, d2);
			double denom = a * e - b * b;
			s = (denom > eps) ? Clamp01((b * f - c * e) / denom) : 0.0;
			t = (b * s + f) / e;
			if (t < 0.0) {
				t = 0.0;
				s = Clamp01(-c / a);
			} else if (t > 1.0) {
				t = 1.0;
				s = Clamp01((b - c) / a);
			}
		}
	}
	Vertex3D c1 {p1.x + d1.x * s, p1.y + d1.y * s, p1.z + d1.z * s};
	Vertex3D c2 {p2.x + d2.x * t, p2.y + d2.y * t, p2.z + d2.z * t};
	return {c1, c2};
}

double DistSegmentSegment(const Vertex3D &p1, const Vertex3D &q1, const Vertex3D &p2, const Vertex3D &q2) {
	auto pair = ClosestPointPairSegmentSegment(p1, q1, p2, q2);
	return DistPointPoint(pair.p, pair.q);
}

//! Intersection point of segment [p,q] with triangle plane, assuming it hits
//! the triangle interior. Computed via Möller–Trumbore.
Vertex3D SegmentTriangleIntersectionPoint(const Vertex3D &p, const Vertex3D &q, const Vertex3D &a, const Vertex3D &b,
                                          const Vertex3D &c) {
	Vec3 dir = Sub(q, p);
	Vec3 e1 = Sub(b, a);
	Vec3 e2 = Sub(c, a);
	Vec3 h = Cross(dir, e2);
	double det = Dot(e1, h);
	if (std::fabs(det) < kEpsAbsolute) {
		return p; // parallel fallback
	}
	double inv = 1.0 / det;
	Vec3 s = Sub(p, a);
	double u = inv * Dot(s, h);
	double v = inv * Dot(Cross(s, e1), dir);
	double w = 1.0 - u - v;
	return Vertex3D {a.x * w + b.x * u + c.x * v, a.y * w + b.y * u + c.y * v, a.z * w + b.z * u + c.z * v};
}

//! Keep the closest of two candidate pairs.
void KeepCloser(ClosestPointPair &best, const ClosestPointPair &candidate) {
	if (DistPointPoint(candidate.p, candidate.q) < DistPointPoint(best.p, best.q)) {
		best = candidate;
	}
}

ClosestPointPair ClosestPointPairSegmentTriangle(const Vertex3D &p, const Vertex3D &q, const Vertex3D &a,
                                                 const Vertex3D &b, const Vertex3D &c) {
	if (SegmentIntersectsTriangle(p, q, a, b, c)) {
		Vertex3D ipt = SegmentTriangleIntersectionPoint(p, q, a, b, c);
		return {ipt, ipt};
	}
	// Endpoint-vs-triangle candidates (segment endpoint maps to p side).
	ClosestPointPair best = {p, ClosestPointOnTriangle(p, a, b, c)};
	KeepCloser(best, {q, ClosestPointOnTriangle(q, a, b, c)});
	// Segment-vs-edge candidates.
	auto e1 = ClosestPointPairSegmentSegment(p, q, a, b);
	KeepCloser(best, e1);
	auto e2 = ClosestPointPairSegmentSegment(p, q, b, c);
	KeepCloser(best, e2);
	auto e3 = ClosestPointPairSegmentSegment(p, q, c, a);
	KeepCloser(best, e3);
	return best;
}

double DistSegmentTriangle(const Vertex3D &p, const Vertex3D &q, const Vertex3D &a, const Vertex3D &b,
                           const Vertex3D &c) {
	auto pair = ClosestPointPairSegmentTriangle(p, q, a, b, c);
	return DistPointPoint(pair.p, pair.q);
}

ClosestPointPair ClosestPointPairTriangleTriangle(const Vertex3D &a1, const Vertex3D &b1, const Vertex3D &c1,
                                                  const Vertex3D &a2, const Vertex3D &b2, const Vertex3D &c2) {
	ClosestPointPair best = ClosestPointPairSegmentTriangle(a1, b1, a2, b2, c2);
	KeepCloser(best, ClosestPointPairSegmentTriangle(b1, c1, a2, b2, c2));
	KeepCloser(best, ClosestPointPairSegmentTriangle(c1, a1, a2, b2, c2));
	// The reverse six checks are covered because ClosestPointPairSegmentTriangle
	// evaluates segment-vs-triangle edges; still run the symmetric edges for
	// robustness in degenerate cases.
	KeepCloser(best, ClosestPointPairSegmentTriangle(a2, b2, a1, b1, c1));
	KeepCloser(best, ClosestPointPairSegmentTriangle(b2, c2, a1, b1, c1));
	KeepCloser(best, ClosestPointPairSegmentTriangle(c2, a2, a1, b1, c1));
	return best;
}

double DistTriangleTriangle(const Vertex3D &a1, const Vertex3D &b1, const Vertex3D &c1, const Vertex3D &a2,
                            const Vertex3D &b2, const Vertex3D &c2) {
	auto pair = ClosestPointPairTriangleTriangle(a1, b1, c1, a2, b2, c2);
	return DistPointPoint(pair.p, pair.q);
}

namespace {

//! A geometric element: a point (n=1), segment (n=2) or triangle (n=3), with
//! its bounding box.
struct Element {
	int n;
	Vertex3D v[3];
	BBox3D box;
};

Element MakeElement(int n, const Vertex3D &a, const Vertex3D &b = {}, const Vertex3D &c = {}) {
	Element e {n, {a, b, c}, {a.x, a.y, a.z, a.x, a.y, a.z}};
	for (int i = 1; i < n; i++) {
		const auto &v = e.v[i];
		e.box.min_x = std::min(e.box.min_x, v.x);
		e.box.min_y = std::min(e.box.min_y, v.y);
		e.box.min_z = std::min(e.box.min_z, v.z);
		e.box.max_x = std::max(e.box.max_x, v.x);
		e.box.max_y = std::max(e.box.max_y, v.y);
		e.box.max_z = std::max(e.box.max_z, v.z);
	}
	return e;
}

//! Fan-triangulate the ring spanning vertex indices [begin,end) into triangles.
void FanTriangulate(const GeomModel &m, uint32_t begin, uint32_t end, std::vector<Element> &out) {
	if (end - begin < 3) {
		return;
	}
	const Vertex3D &v0 = m.vertices[begin];
	for (uint32_t i = begin + 1; i + 1 < end; i++) {
		out.push_back(MakeElement(3, v0, m.vertices[i], m.vertices[i + 1]));
	}
}

//! Decompose a geometry into primitive elements according to its dimension.
//! Surfaces are fan-triangulated over each face's exterior ring (holes are
//! ignored for distance — a documented v1 simplification).
std::vector<Element> Decompose(const GeomModel &m) {
	std::vector<Element> out;
	switch (m.type) {
	case GeomType::Point:
	case GeomType::MultiPoint:
		for (const auto &v : m.vertices) {
			out.push_back(MakeElement(1, v));
		}
		break;
	case GeomType::LineString:
		for (size_t i = 1; i < m.vertices.size(); i++) {
			out.push_back(MakeElement(2, m.vertices[i - 1], m.vertices[i]));
		}
		break;
	case GeomType::MultiLineString:
		for (size_t k = 0; k + 1 < m.part_offsets.size(); k++) {
			for (uint32_t i = m.part_offsets[k] + 1; i < m.part_offsets[k + 1]; i++) {
				out.push_back(MakeElement(2, m.vertices[i - 1], m.vertices[i]));
			}
		}
		break;
	case GeomType::Polygon:
		// Single polygon: ring 0 is the exterior ring.
		if (m.ring_offsets.size() >= 2) {
			FanTriangulate(m, m.ring_offsets[0], m.ring_offsets[1], out);
		}
		break;
	case GeomType::MultiPolygon:
	case GeomType::PolyhedralSurface:
		// Each part's first ring is its exterior boundary.
		for (size_t k = 0; k + 1 < m.part_offsets.size(); k++) {
			uint32_t ring = m.part_offsets[k];
			FanTriangulate(m, m.ring_offsets[ring], m.ring_offsets[ring + 1], out);
		}
		break;
	default:
		// Fall back to treating raw vertices as points.
		for (const auto &v : m.vertices) {
			out.push_back(MakeElement(1, v));
		}
		break;
	}
	return out;
}

//! Distance between two primitive elements, dispatched on their vertex counts.
double ElementDistance(const Element &e1, const Element &e2) {
	// Order so that n1 <= n2 to halve the dispatch table.
	const Element &a = (e1.n <= e2.n) ? e1 : e2;
	const Element &b = (e1.n <= e2.n) ? e2 : e1;
	if (a.n == 1 && b.n == 1) {
		return DistPointPoint(a.v[0], b.v[0]);
	}
	if (a.n == 1 && b.n == 2) {
		return DistPointSegment(a.v[0], b.v[0], b.v[1]);
	}
	if (a.n == 1 && b.n == 3) {
		return DistPointTriangle(a.v[0], b.v[0], b.v[1], b.v[2]);
	}
	if (a.n == 2 && b.n == 2) {
		return DistSegmentSegment(a.v[0], a.v[1], b.v[0], b.v[1]);
	}
	if (a.n == 2 && b.n == 3) {
		return DistSegmentTriangle(a.v[0], a.v[1], b.v[0], b.v[1], b.v[2]);
	}
	return DistTriangleTriangle(a.v[0], a.v[1], a.v[2], b.v[0], b.v[1], b.v[2]);
}

//! Closest point pair between two elements, preserving (on e1, on e2) order.
ClosestPointPair ElementClosestPair(const Element &e1, const Element &e2) {
	switch (e1.n * 10 + e2.n) {
	case 11:
		return {e1.v[0], e2.v[0]};
	case 12:
		return {e1.v[0], ClosestPointOnSegment(e1.v[0], e2.v[0], e2.v[1])};
	case 13:
		return {e1.v[0], ClosestPointOnTriangle(e1.v[0], e2.v[0], e2.v[1], e2.v[2])};
	case 21:
		return {ClosestPointOnSegment(e2.v[0], e1.v[0], e1.v[1]), e2.v[0]};
	case 22:
		return ClosestPointPairSegmentSegment(e1.v[0], e1.v[1], e2.v[0], e2.v[1]);
	case 23:
		return ClosestPointPairSegmentTriangle(e1.v[0], e1.v[1], e2.v[0], e2.v[1], e2.v[2]);
	case 31:
		return {ClosestPointOnTriangle(e2.v[0], e1.v[0], e1.v[1], e1.v[2]), e2.v[0]};
	case 32: {
		auto pair = ClosestPointPairSegmentTriangle(e2.v[0], e2.v[1], e1.v[0], e1.v[1], e1.v[2]);
		return {pair.q, pair.p}; // swap back to (on e1, on e2)
	}
	case 33:
		return ClosestPointPairTriangleTriangle(e1.v[0], e1.v[1], e1.v[2], e2.v[0], e2.v[1], e2.v[2]);
	default:
		return {e1.v[0], e2.v[0]};
	}
}

//! Squared gap between two boxes: a lower bound on the squared distance between
//! anything inside them (0 when they overlap).
double BoxGap2(const BBox3D &a, const BBox3D &b) {
	auto axis_gap = [](double min1, double max1, double min2, double max2) {
		return std::max(0.0, std::max(min1 - max2, min2 - max1));
	};
	double dx = axis_gap(a.min_x, a.max_x, b.min_x, b.max_x);
	double dy = axis_gap(a.min_y, a.max_y, b.min_y, b.max_y);
	double dz = axis_gap(a.min_z, a.max_z, b.min_z, b.max_z);
	return dx * dx + dy * dy + dz * dz;
}

//! Element pairs are skipped when their boxes are farther apart than the bound
//! that matters (the best distance so far, or a threshold). The box gap is a
//! lower bound on the exact distance, but the element distance is computed in
//! doubles; the slack keeps a pair whose rounded distance could still land on or
//! below the bound in the sweep. It scales with the coordinates' magnitude,
//! because that is what the rounding error scales with, and at 1e-9 of it sits
//! far above that error and far below any gap worth pruning.
double PruneSlack(const GeomModel &g1, const GeomModel &g2) {
	double m = 1.0;
	for (const auto *b : {&g1.bbox, &g2.bbox}) {
		for (double v : {b->min_x, b->min_y, b->min_z, b->max_x, b->max_y, b->max_z}) {
			m = std::max(m, std::abs(v));
		}
	}
	return kEpsRelative * m;
}

//! True when the boxes are too far apart for any pair inside them to come
//! within `bound` (plus slack) of each other.
bool BeyondBound(const BBox3D &a, const BBox3D &b, double bound, double slack) {
	double limit = bound + slack;
	return BoxGap2(a, b) > limit * limit;
}

//! The elements of `elements` whose boxes come within `bound` (plus slack) of
//! `box`, in their original order: the only ones a pair under that bound can
//! use, filtered once rather than re-tested for every element on the other side.
std::vector<const Element *> WithinBound(const std::vector<Element> &elements, const BBox3D &box, double bound,
                                         double slack) {
	std::vector<const Element *> out;
	for (const auto &e : elements) {
		if (!BeyondBound(e.box, box, bound, slack)) {
			out.push_back(&e);
		}
	}
	return out;
}

//! An upper bound on the minimum element distance: the exact distance of one
//! element pair, picked to be close. It is a real element distance, so the true
//! minimum is at most this, and every pair that could attain the minimum has a
//! box gap no larger than it. The pair is found in linear time by alternating:
//! the element of `e1` whose box is closest to `g2`'s, then the element of `e2`
//! closest to that one, then back once more.
double SeedUpperBound(const std::vector<Element> &e1, const std::vector<Element> &e2, const BBox3D &g2_box) {
	auto closest_to = [](const std::vector<Element> &elements, const BBox3D &box) {
		size_t best = 0;
		double best_gap2 = std::numeric_limits<double>::infinity();
		for (size_t i = 0; i < elements.size(); i++) {
			double g = BoxGap2(elements[i].box, box);
			if (g < best_gap2) {
				best_gap2 = g;
				best = i;
			}
		}
		return best;
	};
	size_t bi = closest_to(e1, g2_box);
	size_t bj = closest_to(e2, e1[bi].box);
	bi = closest_to(e1, e2[bj].box);
	bj = closest_to(e2, e1[bi].box);
	// A non-finite coordinate gives a NaN distance, which bounds nothing; the
	// sweep then prunes nothing and treats that pair as the unpruned loop does.
	double d = ElementDistance(e1[bi], e2[bj]);
	return std::isnan(d) ? std::numeric_limits<double>::infinity() : d;
}

} // namespace

double Geom3DMaxDistance(const GeomModel &g1, const GeomModel &g2) {
	// The maximum distance between two bounded geometries is attained at a pair
	// of vertices, so a vertex/vertex sweep is exact.
	double best = 0.0;
	for (const auto &a : g1.vertices) {
		for (const auto &b : g2.vertices) {
			best = std::max(best, DistPointPoint(a, b));
		}
	}
	return best;
}

double Geom3DDistance(const GeomModel &g1, const GeomModel &g2) {
	auto e1 = Decompose(g1);
	auto e2 = Decompose(g2);
	if (e1.empty() || e2.empty()) {
		return std::numeric_limits<double>::infinity();
	}
	// The minimum over every element pair, skipping the pairs whose boxes are
	// already farther apart than the best distance found: they cannot lower it.
	// Seeding with a close pair makes the bound tight from the start.
	double slack = PruneSlack(g1, g2);
	double best = SeedUpperBound(e1, e2, g2.bbox);
	if (best == 0.0) {
		return 0.0;
	}
	auto candidates = WithinBound(e2, g1.bbox, best, slack);
	for (const auto &a : e1) {
		if (BeyondBound(a.box, g2.bbox, best, slack)) {
			continue;
		}
		for (const auto *b : candidates) {
			if (BeyondBound(a.box, b->box, best, slack)) {
				continue;
			}
			best = std::min(best, ElementDistance(a, *b));
			if (best == 0.0) {
				return 0.0;
			}
		}
	}
	return best;
}

double BBoxDistance(const BBox3D &a, const BBox3D &b) {
	// Per-axis gap between the two boxes; 0 when they overlap on that axis.
	auto axis_gap = [](double min1, double max1, double min2, double max2) {
		if (min1 > max2) {
			return min1 - max2;
		}
		if (min2 > max1) {
			return min2 - max1;
		}
		return 0.0;
	};
	double dx = axis_gap(a.min_x, a.max_x, b.min_x, b.max_x);
	double dy = axis_gap(a.min_y, a.max_y, b.min_y, b.max_y);
	double dz = axis_gap(a.min_z, a.max_z, b.min_z, b.max_z);
	return std::sqrt(dx * dx + dy * dy + dz * dz);
}

double Geom3DBBoxDistance(const GeomModel &g1, const GeomModel &g2) {
	return BBoxDistance(g1.bbox, g2.bbox);
}

bool Geom3DWithin(const GeomModel &g1, const GeomModel &g2, double threshold) {
	// Cheap rejection: if even the bbox lower bound exceeds the threshold, no
	// element pair can be within it, so skip decomposition entirely.
	if (Geom3DBBoxDistance(g1, g2) > threshold) {
		return false;
	}
	auto e1 = Decompose(g1);
	auto e2 = Decompose(g2);
	double slack = PruneSlack(g1, g2);
	auto candidates = WithinBound(e2, g1.bbox, threshold, slack);
	for (const auto &a : e1) {
		if (BeyondBound(a.box, g2.bbox, threshold, slack)) {
			continue;
		}
		for (const auto *b : candidates) {
			// A pair whose boxes are beyond the threshold cannot be within it.
			if (BeyondBound(a.box, b->box, threshold, slack)) {
				continue;
			}
			// Stop at the first pair within the threshold — no need to find the
			// exact minimum distance.
			if (ElementDistance(a, *b) <= threshold) {
				return true;
			}
		}
	}
	return false;
}

ClosestPointPair Geom3DClosestPoints(const GeomModel &g1, const GeomModel &g2) {
	auto e1 = Decompose(g1);
	auto e2 = Decompose(g2);
	if (e1.empty() || e2.empty()) {
		return {{0, 0, 0}, {0, 0, 0}};
	}
	// The result is the first pair, in sweep order, that attains the minimum.
	// A pair whose boxes are farther apart than an upper bound on the minimum
	// cannot be that pair, so it is skipped without changing which one wins.
	double slack = PruneSlack(g1, g2);
	double upper = SeedUpperBound(e1, e2, g2.bbox);
	ClosestPointPair best = ElementClosestPair(e1[0], e2[0]);
	double best_dist = DistPointPoint(best.p, best.q);
	auto candidates = WithinBound(e2, g1.bbox, std::min(upper, best_dist), slack);
	for (const auto &a : e1) {
		if (BeyondBound(a.box, g2.bbox, std::min(upper, best_dist), slack)) {
			continue;
		}
		for (const auto *b : candidates) {
			if (BeyondBound(a.box, b->box, std::min(upper, best_dist), slack)) {
				continue;
			}
			auto candidate = ElementClosestPair(a, *b);
			double d = DistPointPoint(candidate.p, candidate.q);
			if (d < best_dist) {
				best = candidate;
				best_dist = d;
				if (best_dist == 0.0) {
					return best;
				}
			}
		}
	}
	return best;
}

} // namespace duckdb_3d
