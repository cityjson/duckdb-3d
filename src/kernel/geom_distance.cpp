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
bool SegmentIntersectsTriangle(const Vertex3D &p, const Vertex3D &q, const Vertex3D &a,
                               const Vertex3D &b, const Vertex3D &c) {
	const double eps = 1e-12;
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

double DistPointSegment(const Vertex3D &p, const Vertex3D &a, const Vertex3D &b) {
	Vec3 ab = Sub(b, a);
	Vec3 ap = Sub(p, a);
	double len2 = Dot(ab, ab);
	double t = (len2 > 0.0) ? Clamp01(Dot(ap, ab) / len2) : 0.0;
	Vertex3D proj{a.x + ab.x * t, a.y + ab.y * t, a.z + ab.z * t};
	return DistPointPoint(p, proj);
}

double DistPointTriangle(const Vertex3D &p, const Vertex3D &a, const Vertex3D &b,
                         const Vertex3D &c) {
	// Ericson, Real-Time Collision Detection §5.1.5 (ClosestPtPointTriangle).
	Vec3 ab = Sub(b, a);
	Vec3 ac = Sub(c, a);
	Vec3 ap = Sub(p, a);
	double d1 = Dot(ab, ap);
	double d2 = Dot(ac, ap);
	auto at = [&](double u, double v, double w) {
		// Barycentric (u,v,w) → cartesian closest point on triangle.
		return Vertex3D{a.x * u + b.x * v + c.x * w, a.y * u + b.y * v + c.y * w,
		                a.z * u + b.z * v + c.z * w};
	};
	if (d1 <= 0.0 && d2 <= 0.0) {
		return DistPointPoint(p, a); // vertex region a
	}
	Vec3 bp = Sub(p, b);
	double d3 = Dot(ab, bp);
	double d4 = Dot(ac, bp);
	if (d3 >= 0.0 && d4 <= d3) {
		return DistPointPoint(p, b); // vertex region b
	}
	double vc = d1 * d4 - d3 * d2;
	if (vc <= 0.0 && d1 >= 0.0 && d3 <= 0.0) {
		double v = d1 / (d1 - d3);
		return DistPointPoint(p, at(1.0 - v, v, 0.0)); // edge ab
	}
	Vec3 cp = Sub(p, c);
	double d5 = Dot(ab, cp);
	double d6 = Dot(ac, cp);
	if (d6 >= 0.0 && d5 <= d6) {
		return DistPointPoint(p, c); // vertex region c
	}
	double vb = d5 * d2 - d1 * d6;
	if (vb <= 0.0 && d2 >= 0.0 && d6 <= 0.0) {
		double w = d2 / (d2 - d6);
		return DistPointPoint(p, at(1.0 - w, 0.0, w)); // edge ac
	}
	double va = d3 * d6 - d5 * d4;
	if (va <= 0.0 && (d4 - d3) >= 0.0 && (d5 - d6) >= 0.0) {
		double w = (d4 - d3) / ((d4 - d3) + (d5 - d6));
		return DistPointPoint(p, at(0.0, 1.0 - w, w)); // edge bc
	}
	// Interior: project onto the face plane via barycentric coords.
	double denom = 1.0 / (va + vb + vc);
	double v = vb * denom;
	double w = vc * denom;
	return DistPointPoint(p, at(1.0 - v - w, v, w));
}

double DistSegmentSegment(const Vertex3D &p1, const Vertex3D &q1, const Vertex3D &p2,
                          const Vertex3D &q2) {
	// Ericson, Real-Time Collision Detection §5.1.9 (ClosestPtSegmentSegment).
	Vec3 d1 = Sub(q1, p1); // direction of segment 1
	Vec3 d2 = Sub(q2, p2); // direction of segment 2
	Vec3 r = Sub(p1, p2);
	double a = Dot(d1, d1);
	double e = Dot(d2, d2);
	double f = Dot(d2, r);

	double s, t;
	const double eps = 1e-12;
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
	Vertex3D c1{p1.x + d1.x * s, p1.y + d1.y * s, p1.z + d1.z * s};
	Vertex3D c2{p2.x + d2.x * t, p2.y + d2.y * t, p2.z + d2.z * t};
	return DistPointPoint(c1, c2);
}

double DistSegmentTriangle(const Vertex3D &p, const Vertex3D &q, const Vertex3D &a,
                           const Vertex3D &b, const Vertex3D &c) {
	if (SegmentIntersectsTriangle(p, q, a, b, c)) {
		return 0.0;
	}
	// Otherwise the minimum is attained at a segment endpoint vs. the triangle,
	// or between the segment and one of the triangle's edges.
	double best = DistPointTriangle(p, a, b, c);
	best = std::min(best, DistPointTriangle(q, a, b, c));
	best = std::min(best, DistSegmentSegment(p, q, a, b));
	best = std::min(best, DistSegmentSegment(p, q, b, c));
	best = std::min(best, DistSegmentSegment(p, q, c, a));
	return best;
}

double DistTriangleTriangle(const Vertex3D &a1, const Vertex3D &b1, const Vertex3D &c1,
                            const Vertex3D &a2, const Vertex3D &b2, const Vertex3D &c2) {
	// Closest points between two non-intersecting triangles lie on their edges;
	// segment/triangle piercing inside DistSegmentTriangle yields 0 on intersection.
	double best = DistSegmentTriangle(a1, b1, a2, b2, c2);
	best = std::min(best, DistSegmentTriangle(b1, c1, a2, b2, c2));
	best = std::min(best, DistSegmentTriangle(c1, a1, a2, b2, c2));
	best = std::min(best, DistSegmentTriangle(a2, b2, a1, b1, c1));
	best = std::min(best, DistSegmentTriangle(b2, c2, a1, b1, c1));
	best = std::min(best, DistSegmentTriangle(c2, a2, a1, b1, c1));
	return best;
}

namespace {

//! A geometric element: a point (n=1), segment (n=2) or triangle (n=3).
struct Element {
	int n;
	Vertex3D v[3];
};

//! Fan-triangulate the ring spanning vertex indices [begin,end) into triangles.
void FanTriangulate(const GeomModel &m, uint32_t begin, uint32_t end,
                    std::vector<Element> &out) {
	if (end - begin < 3) {
		return;
	}
	const Vertex3D &v0 = m.vertices[begin];
	for (uint32_t i = begin + 1; i + 1 < end; i++) {
		out.push_back({3, {v0, m.vertices[i], m.vertices[i + 1]}});
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
			out.push_back({1, {v}});
		}
		break;
	case GeomType::LineString:
		for (size_t i = 1; i < m.vertices.size(); i++) {
			out.push_back({2, {m.vertices[i - 1], m.vertices[i]}});
		}
		break;
	case GeomType::MultiLineString:
		for (size_t k = 0; k + 1 < m.part_offsets.size(); k++) {
			for (uint32_t i = m.part_offsets[k] + 1; i < m.part_offsets[k + 1]; i++) {
				out.push_back({2, {m.vertices[i - 1], m.vertices[i]}});
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
			out.push_back({1, {v}});
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
	double best = std::numeric_limits<double>::infinity();
	for (const auto &a : e1) {
		for (const auto &b : e2) {
			best = std::min(best, ElementDistance(a, b));
			if (best == 0.0) {
				return 0.0;
			}
		}
	}
	return best;
}

} // namespace duckdb_3d
