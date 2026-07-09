#include "kernel/geom_analysis.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace duckdb_3d {

namespace {

struct Vec3 {
	double x, y, z;
};

Vec3 MakeVec3(const Vertex3D &v) {
	return {v.x, v.y, v.z};
}

Vertex3D MakeVertex(const Vec3 &v) {
	return {v.x, v.y, v.z};
}

Vec3 SubV(const Vec3 &a, const Vec3 &b) {
	return {a.x - b.x, a.y - b.y, a.z - b.z};
}

Vec3 Sub(const Vertex3D &a, const Vertex3D &b) {
	return {a.x - b.x, a.y - b.y, a.z - b.z};
}

Vec3 Add(const Vec3 &a, const Vec3 &b) {
	return {a.x + b.x, a.y + b.y, a.z + b.z};
}

Vec3 Mul(const Vec3 &a, double s) {
	return {a.x * s, a.y * s, a.z * s};
}

double Dot(const Vec3 &a, const Vec3 &b) {
	return a.x * b.x + a.y * b.y + a.z * b.z;
}

Vec3 Cross(const Vec3 &a, const Vec3 &b) {
	return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

double Length(const Vec3 &v) {
	return std::sqrt(Dot(v, v));
}

//! Whether the vertices [begin,end) of `m` are coplanar within tolerance.
bool RingIsPlanar(const GeomModel &m, uint32_t begin, uint32_t end) {
	uint32_t n = end - begin;
	if (n <= 3) {
		return true; // three or fewer points are always coplanar
	}
	// Newell's robust polygon normal and the vertex centroid.
	double nx = 0, ny = 0, nz = 0;
	double cx = 0, cy = 0, cz = 0;
	double scale = 1.0;
	for (uint32_t i = begin; i < end; i++) {
		const auto &a = m.vertices[i];
		const auto &b = m.vertices[(i + 1 < end) ? i + 1 : begin];
		nx += (a.y - b.y) * (a.z + b.z);
		ny += (a.z - b.z) * (a.x + b.x);
		nz += (a.x - b.x) * (a.y + b.y);
		cx += a.x;
		cy += a.y;
		cz += a.z;
		scale = std::max({scale, std::fabs(a.x), std::fabs(a.y), std::fabs(a.z)});
	}
	double len = std::sqrt(nx * nx + ny * ny + nz * nz);
	if (len < 1e-12) {
		return true; // degenerate / collinear → treat as planar (a line)
	}
	nx /= len;
	ny /= len;
	nz /= len;
	cx /= n;
	cy /= n;
	cz /= n;
	double tol = 1e-9 * scale;
	for (uint32_t i = begin; i < end; i++) {
		const auto &v = m.vertices[i];
		double d = (v.x - cx) * nx + (v.y - cy) * ny + (v.z - cz) * nz;
		if (std::fabs(d) > tol) {
			return false;
		}
	}
	return true;
}

} // namespace

bool Geom3DIsPlanar(const GeomModel &geom) {
	switch (geom.type) {
	case GeomType::Polygon:
	case GeomType::MultiPolygon:
	case GeomType::PolyhedralSurface:
		// Every ring (each face's rings) must be planar.
		for (size_t r = 0; r + 1 < geom.ring_offsets.size(); r++) {
			if (!RingIsPlanar(geom, geom.ring_offsets[r], geom.ring_offsets[r + 1])) {
				return false;
			}
		}
		return true;
	default:
		// Points and (multi)lines: the whole vertex list as one set.
		return RingIsPlanar(geom, 0, static_cast<uint32_t>(geom.vertices.size()));
	}
}

// ──────────────────────────────────────────────────────────────
// Centroid helpers
// ──────────────────────────────────────────────────────────────

namespace {

//! Build a right-handed orthonormal basis (u,v) for the plane with normal n,
//! picking the axis-aligned direction most perpendicular to n as u.
void PlaneBasis(const Vec3 &n, Vec3 &u, Vec3 &v) {
	Vec3 candidate = {1, 0, 0};
	if (std::fabs(n.x) > 0.9) {
		candidate = {0, 1, 0};
	}
	u = Cross(n, candidate);
	double len = Length(u);
	if (len > 1e-12) {
		u = Mul(u, 1.0 / len);
	} else {
		u = {0, 1, 0};
	}
	v = Cross(n, u);
}

//! Centroid and signed area of a planar polygon ring in its own 3D plane.
//! Returns (area, centroid); area may be negative for clockwise rings.
std::pair<double, Vertex3D> RingCentroid(const GeomModel &m, uint32_t begin, uint32_t end) {
	uint32_t n = end - begin;
	if (n == 0) {
		return std::make_pair(0.0, Vertex3D {0, 0, 0});
	}
	if (n == 1) {
		return std::make_pair(0.0, m.vertices[begin]);
	}

	// Compute robust plane normal via Newell's method.
	Vec3 nrm = {0, 0, 0};
	for (uint32_t i = begin; i < end; i++) {
		const auto &a = m.vertices[i];
		const auto &b = m.vertices[(i + 1 < end) ? i + 1 : begin];
		nrm.x += (a.y - b.y) * (a.z + b.z);
		nrm.y += (a.z - b.z) * (a.x + b.x);
		nrm.z += (a.x - b.x) * (a.y + b.y);
	}
	double nlen = Length(nrm);
	if (nlen < 1e-12) {
		// Degenerate ring: fall back to vertex average.
		Vec3 sum = {0, 0, 0};
		for (uint32_t i = begin; i < end; i++) {
			sum = Add(sum, MakeVec3(m.vertices[i]));
		}
		return std::make_pair(0.0, MakeVertex(Mul(sum, 1.0 / n)));
	}
	nrm = Mul(nrm, 1.0 / nlen);

	Vec3 u, v;
	PlaneBasis(nrm, u, v);

	// Use first vertex as origin for numerical stability.
	Vec3 origin = MakeVec3(m.vertices[begin]);
	double area = 0.0;
	double cx = 0.0, cy = 0.0;
	for (uint32_t i = begin; i < end; i++) {
		const auto &a = m.vertices[i];
		const auto &b = m.vertices[(i + 1 < end) ? i + 1 : begin];
		Vec3 da = SubV(MakeVec3(a), origin);
		Vec3 db = SubV(MakeVec3(b), origin);
		double s_a = Dot(da, u);
		double t_a = Dot(da, v);
		double s_b = Dot(db, u);
		double t_b = Dot(db, v);
		double cross = s_a * t_b - s_b * t_a;
		area += cross;
		cx += cross * (s_a + s_b);
		cy += cross * (t_a + t_b);
	}
	if (std::fabs(area) < 1e-18) {
		return std::make_pair(0.0, MakeVertex(origin));
	}
	double inv_area = 1.0 / (3.0 * area);
	Vec3 centroid_3d = Add(origin, Add(Mul(u, cx * inv_area), Mul(v, cy * inv_area)));
	return std::make_pair(0.5 * area, MakeVertex(centroid_3d));
}

//! Centroid of one polygonal face (exterior ring only for v1).
Vertex3D FaceCentroid(const GeomModel &m, uint32_t ring_begin, uint32_t ring_end) {
	std::pair<double, Vertex3D> result = RingCentroid(m, ring_begin, ring_end);
	return result.second;
}

//! Length-weighted centroid of a polyline spanning vertex indices [begin,end).
Vertex3D PolylineCentroid(const GeomModel &m, uint32_t begin, uint32_t end) {
	if (end <= begin + 1) {
		return m.vertices[begin];
	}
	Vec3 weighted = {0, 0, 0};
	double total_len = 0.0;
	for (uint32_t i = begin + 1; i < end; i++) {
		Vec3 a = MakeVec3(m.vertices[i - 1]);
		Vec3 b = MakeVec3(m.vertices[i]);
		Vec3 seg = SubV(b, a);
		double len = Length(seg);
		if (len > 0.0) {
			weighted = Add(weighted, Mul(Add(a, b), 0.5 * len));
			total_len += len;
		}
	}
	if (total_len == 0.0) {
		return m.vertices[begin];
	}
	return MakeVertex(Mul(weighted, 1.0 / total_len));
}

} // namespace

Vertex3D Geom3DCentroid(const GeomModel &geom) {
	switch (geom.type) {
	case GeomType::Point:
	case GeomType::MultiPoint: {
		if (geom.vertices.empty()) {
			return {0, 0, 0};
		}
		Vec3 sum = {0, 0, 0};
		for (const auto &v : geom.vertices) {
			sum = Add(sum, MakeVec3(v));
		}
		return MakeVertex(Mul(sum, 1.0 / static_cast<double>(geom.vertices.size())));
	}
	case GeomType::LineString:
		return PolylineCentroid(geom, 0, static_cast<uint32_t>(geom.vertices.size()));
	case GeomType::MultiLineString: {
		Vec3 weighted = {0, 0, 0};
		double total_len = 0.0;
		for (size_t k = 0; k + 1 < geom.part_offsets.size(); k++) {
			Vertex3D part_c = PolylineCentroid(geom, geom.part_offsets[k], geom.part_offsets[k + 1]);
			double part_len = 0.0;
			for (uint32_t i = geom.part_offsets[k] + 1; i < geom.part_offsets[k + 1]; i++) {
				part_len += Length(Sub(geom.vertices[i], geom.vertices[i - 1]));
			}
			weighted = Add(weighted, Mul(MakeVec3(part_c), part_len));
			total_len += part_len;
		}
		if (total_len == 0.0) {
			return geom.vertices.empty() ? Vertex3D {0, 0, 0} : geom.vertices[0];
		}
		return MakeVertex(Mul(weighted, 1.0 / total_len));
	}
	case GeomType::Polygon: {
		if (geom.ring_offsets.size() < 2) {
			return {0, 0, 0};
		}
		// Exterior ring only for v1 (single-ring common case).
		return FaceCentroid(geom, geom.ring_offsets[0], geom.ring_offsets[1]);
	}
	case GeomType::MultiPolygon:
	case GeomType::PolyhedralSurface: {
		if (geom.part_offsets.size() < 2) {
			return {0, 0, 0};
		}
		Vec3 weighted = {0, 0, 0};
		double total_area = 0.0;
		for (size_t k = 0; k + 1 < geom.part_offsets.size(); k++) {
			uint32_t ring = geom.part_offsets[k];
			std::pair<double, Vertex3D> result =
			    RingCentroid(geom, geom.ring_offsets[ring], geom.ring_offsets[ring + 1]);
			double area = std::fabs(result.first);
			weighted = Add(weighted, Mul(MakeVec3(result.second), area));
			total_area += area;
		}
		if (total_area == 0.0) {
			return geom.vertices.empty() ? Vertex3D {0, 0, 0} : geom.vertices[0];
		}
		return MakeVertex(Mul(weighted, 1.0 / total_area));
	}
	default:
		return {0, 0, 0};
	}
}

// ──────────────────────────────────────────────────────────────
// Convex hull (2D XY)
// ──────────────────────────────────────────────────────────────

namespace {

//! 2D cross product (b-a) × (c-a); positive ⇒ c is left of ab.
double Cross2D(const Vertex3D &a, const Vertex3D &b, const Vertex3D &c) {
	return (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
}

} // namespace

GeomModel Geom3DConvexHull(const GeomModel &geom) {
	if (geom.vertices.empty()) {
		GeomModel empty;
		empty.type = GeomType::Point;
		empty.ComputeBBox();
		return empty;
	}

	// Collect unique XY points, remembering min Z.
	double min_z = geom.bbox.min_z;
	std::vector<Vertex3D> pts = geom.vertices;
	std::sort(pts.begin(), pts.end(), [](const Vertex3D &a, const Vertex3D &b) {
		if (a.x != b.x) {
			return a.x < b.x;
		}
		return a.y < b.y;
	});
	pts.erase(std::unique(pts.begin(), pts.end(),
	                      [](const Vertex3D &a, const Vertex3D &b) { return a.x == b.x && a.y == b.y; }),
	          pts.end());

	if (pts.size() == 1) {
		GeomModel out;
		out.type = GeomType::Point;
		out.vertices.push_back({pts[0].x, pts[0].y, min_z});
		out.ComputeBBox();
		return out;
	}

	// Monotone chain.
	std::vector<Vertex3D> lower, upper;
	for (const auto &p : pts) {
		while (lower.size() >= 2 && Cross2D(lower[lower.size() - 2], lower.back(), p) <= 0) {
			lower.pop_back();
		}
		lower.push_back(p);
	}
	for (size_t i = pts.size(); i-- > 0;) {
		const auto &p = pts[i];
		while (upper.size() >= 2 && Cross2D(upper[upper.size() - 2], upper.back(), p) <= 0) {
			upper.pop_back();
		}
		upper.push_back(p);
	}
	// Concatenate lower and upper, removing duplicate endpoints.
	lower.pop_back();
	upper.pop_back();
	std::vector<Vertex3D> hull = lower;
	hull.insert(hull.end(), upper.begin(), upper.end());

	for (auto &v : hull) {
		v.z = min_z;
	}

	GeomModel out;
	if (hull.size() == 2) {
		out.type = GeomType::LineString;
		out.vertices = hull;
	} else {
		out.type = GeomType::Polygon;
		out.vertices = hull;
		out.ring_offsets = {0, static_cast<uint32_t>(hull.size())};
	}
	out.ComputeBBox();
	return out;
}

} // namespace duckdb_3d
