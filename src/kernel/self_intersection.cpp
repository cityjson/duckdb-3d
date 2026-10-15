#include "kernel/self_intersection.hpp"
#include "kernel/geometry_math.hpp"
#include <algorithm>
#include <cmath>
#include <vector>

namespace duckdb_3d {

namespace {

Vertex3D Sub(const Vertex3D &a, const Vertex3D &b) {
	return {a.x - b.x, a.y - b.y, a.z - b.z};
}

double Dot(const Vertex3D &a, const Vertex3D &b) {
	return a.x * b.x + a.y * b.y + a.z * b.z;
}

Vertex3D Cross(const Vertex3D &a, const Vertex3D &b) {
	return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

Vertex3D Scale(const Vertex3D &a, double s) {
	return {a.x * s, a.y * s, a.z * s};
}

double Norm(const Vertex3D &a) {
	return std::sqrt(Dot(a, a));
}

//! The smallest crossing that counts, relative to the solid's bounding-box
//! diagonal: below a hundred-thousandth of a solid's size a crossing is the
//! rounding of its coordinates, not its shape. City models publish millimetre
//! coordinates for objects tens of metres across, where two faces meeting at
//! a vertex can cross by half a millimetre for no other reason.
constexpr double kSelfIntersectionRelative = 1e-5;

//! A face's plane: the unit Newell normal through the centroid of its
//! vertices, and how far its vertices stray from it.
struct FacePlane {
	Vertex3D normal;
	double offset;
	double deviation;
	bool defined;
};

FacePlane FitFacePlane(const SolidModel &model, uint32_t face, const Vertex3D &origin) {
	Vertex3D area = {0, 0, 0};
	Vertex3D centroid = {0, 0, 0};
	uint32_t count = 0;
	for (uint32_t r = model.face_ring_offsets[face]; r < model.face_ring_offsets[face + 1]; r++) {
		auto ring_area = NewellRingAreaVector(model, r);
		area = {area.x + ring_area.x, area.y + ring_area.y, area.z + ring_area.z};
		for (uint32_t i = model.ring_vertex_offsets[r]; i < model.ring_vertex_offsets[r + 1]; i++) {
			auto v = Sub(model.vertices[model.ring_vertex_indices[i]], origin);
			centroid = {centroid.x + v.x, centroid.y + v.y, centroid.z + v.z};
			count++;
		}
	}
	double length = Norm(area);
	if (count == 0 || !(length > 0.0)) {
		return {{0, 0, 0}, 0.0, 0.0, false};
	}
	FacePlane plane;
	plane.normal = Scale(area, 1.0 / length);
	plane.offset = Dot(plane.normal, Scale(centroid, 1.0 / count));
	plane.deviation = 0.0;
	plane.defined = true;
	for (uint32_t r = model.face_ring_offsets[face]; r < model.face_ring_offsets[face + 1]; r++) {
		for (uint32_t i = model.ring_vertex_offsets[r]; i < model.ring_vertex_offsets[r + 1]; i++) {
			auto v = Sub(model.vertices[model.ring_vertex_indices[i]], origin);
			plane.deviation = std::max(plane.deviation, std::abs(Dot(plane.normal, v) - plane.offset));
		}
	}
	return plane;
}

//! A triangle in solid-local coordinates with its face's plane and deviation,
//! its share of a pair's tolerance (twice its face's deviation, plus half the
//! feature floor) and its bounding box grown by that share.
struct Triangle {
	Vertex3D p[3];
	Vertex3D normal;
	double offset;
	double deviation;
	Vertex3D own_normal;
	double lo[3];
	double hi[3];
	double tolerance;
	uint32_t face;
};

//! Whether signed distances reach further than `tolerance` on both sides.
bool Straddles(const double d[3], double tolerance) {
	return std::max({d[0], d[1], d[2]}) > tolerance && std::min({d[0], d[1], d[2]}) < -tolerance;
}

//! The interval, along unit direction `dir`, of the segment where a triangle
//! with signed plane distances `d` (both signs present) meets the plane.
void CrossingInterval(const Triangle &t, const double d[3], const Vertex3D &dir, double &lo, double &hi) {
	lo = INFINITY;
	hi = -INFINITY;
	for (int i = 0; i < 3; i++) {
		int j = (i + 1) % 3;
		if ((d[i] > 0) == (d[j] > 0)) {
			continue;
		}
		double s = d[i] / (d[i] - d[j]);
		Vertex3D p = {t.p[i].x + (t.p[j].x - t.p[i].x) * s, t.p[i].y + (t.p[j].y - t.p[i].y) * s,
		              t.p[i].z + (t.p[j].z - t.p[i].z) * s};
		double along = Dot(p, dir);
		lo = std::min(lo, along);
		hi = std::max(hi, along);
	}
}

struct Point2 {
	double x;
	double y;
};

double Cross2(const Point2 &o, const Point2 &a, const Point2 &b) {
	return (a.x - o.x) * (b.y - o.y) - (a.y - o.y) * (b.x - o.x);
}

//! Width (twice the area over the perimeter) of the overlap of two triangles
//! projected into the plane of `reference`'s face.
double CoplanarOverlapWidth(const Triangle &reference, const Triangle &other) {
	auto edge = Sub(reference.p[1], reference.p[0]);
	auto u = Sub(edge, Scale(reference.normal, Dot(reference.normal, edge)));
	double u_length = Norm(u);
	if (!(u_length > 0.0)) {
		return 0.0;
	}
	u = Scale(u, 1.0 / u_length);
	auto v = Cross(reference.normal, u);
	auto project = [&](const Vertex3D &p) {
		auto q = Sub(p, reference.p[0]);
		return Point2 {Dot(q, u), Dot(q, v)};
	};
	Point2 clip[3] = {project(reference.p[0]), project(reference.p[1]), project(reference.p[2])};
	if (Cross2(clip[0], clip[1], clip[2]) < 0) {
		std::swap(clip[1], clip[2]);
	}
	std::vector<Point2> poly = {project(other.p[0]), project(other.p[1]), project(other.p[2])};
	std::vector<Point2> next;
	for (int e = 0; e < 3 && !poly.empty(); e++) {
		const Point2 &a = clip[e];
		const Point2 &b = clip[(e + 1) % 3];
		next.clear();
		for (size_t i = 0; i < poly.size(); i++) {
			const Point2 &p = poly[i];
			const Point2 &q = poly[(i + 1) % poly.size()];
			double sp = Cross2(a, b, p);
			double sq = Cross2(a, b, q);
			if (sp >= 0) {
				next.push_back(p);
			}
			if ((sp >= 0) != (sq >= 0)) {
				double s = sp / (sp - sq);
				next.push_back({p.x + (q.x - p.x) * s, p.y + (q.y - p.y) * s});
			}
		}
		poly.swap(next);
	}
	if (poly.size() < 3) {
		return 0.0;
	}
	// Shoelace about the polygon's own first point: about the origin, a clipped
	// sliver a few ulps across far from it would keep the rounding of its
	// coordinates' products as area.
	double area2 = 0.0;
	double perimeter = 0.0;
	for (size_t i = 0; i < poly.size(); i++) {
		const Point2 &p = poly[i];
		const Point2 &q = poly[(i + 1) % poly.size()];
		area2 += Cross2(poly[0], p, q);
		perimeter += std::hypot(q.x - p.x, q.y - p.y);
	}
	return perimeter > 0.0 ? std::abs(area2) / perimeter : 0.0;
}

//! Three separate decisions, each with its own number. Coplanar: one
//! triangle lies on the other's face plane as closely as the two faces' own
//! vertices lie on their planes (their deviations, plus the rounding term
//! `eps`), so a triangle folded back onto a warped neighbour is coplanar
//! with it; then they cross when their overlap in that plane is wider than
//! the tolerance. Otherwise each must reach further than the tolerance to both
//! sides of the other's face plane: depth is measured against face planes,
//! so a sliver triangle's ill-defined plane cannot make a neighbour look
//! deep. Where they cross is measured on the triangles themselves, whose
//! shared vertices lie exactly on both planes: the two segments where each
//! triangle meets the other's plane must overlap by more than the tolerance.
//! On success with `segment` set, the overlap of the two crossing segments
//! (its ends); for a coplanar overlap `segment` is false.
bool TrianglesCross(const Triangle &a, const Triangle &b, double tolerance, double eps, bool &segment, Vertex3D &s0,
                    Vertex3D &s1) {
	segment = false;
	double db[3], da[3];
	for (int i = 0; i < 3; i++) {
		db[i] = Dot(a.normal, b.p[i]) - a.offset;
		da[i] = Dot(b.normal, a.p[i]) - b.offset;
	}
	const double coplanar = a.deviation + b.deviation + eps;
	bool b_on_a = std::max({std::abs(db[0]), std::abs(db[1]), std::abs(db[2])}) <= coplanar;
	bool a_on_b = std::max({std::abs(da[0]), std::abs(da[1]), std::abs(da[2])}) <= coplanar;
	if (b_on_a || a_on_b) {
		return b_on_a ? CoplanarOverlapWidth(a, b) > tolerance : CoplanarOverlapWidth(b, a) > tolerance;
	}
	if (!Straddles(da, tolerance) || !Straddles(db, tolerance)) {
		return false;
	}
	double ta[3], tb[3];
	for (int i = 0; i < 3; i++) {
		tb[i] = Dot(a.own_normal, Sub(b.p[i], a.p[0]));
		ta[i] = Dot(b.own_normal, Sub(a.p[i], b.p[0]));
	}
	if (!Straddles(ta, 0.0) || !Straddles(tb, 0.0)) {
		return false;
	}
	auto dir = Cross(a.own_normal, b.own_normal);
	double dir_length = Norm(dir);
	if (!(dir_length > 0.0)) {
		return false;
	}
	dir = Scale(dir, 1.0 / dir_length);
	double a_lo, a_hi, b_lo, b_hi;
	CrossingInterval(a, ta, dir, a_lo, a_hi);
	CrossingInterval(b, tb, dir, b_lo, b_hi);
	double lo = std::max(a_lo, b_lo), hi = std::min(a_hi, b_hi);
	if (!(hi - lo > tolerance)) {
		return false;
	}
	// A point on the line where the two triangle planes meet: the solution of
	// n_a . x = n_a . a0, n_b . x = n_b . b0 nearest the origin, then moved
	// along `dir` to the overlap's ends.
	const Vertex3D &na = a.own_normal, &nb = b.own_normal;
	double ha = Dot(na, a.p[0]), hb = Dot(nb, b.p[0]);
	double nab = Dot(na, nb), det = 1.0 - nab * nab;
	double ca = (ha - hb * nab) / det, cb = (hb - ha * nab) / det;
	Vertex3D base = {ca * na.x + cb * nb.x, ca * na.y + cb * nb.y, ca * na.z + cb * nb.z};
	double base_along = Dot(base, dir);
	s0 = {base.x + dir.x * (lo - base_along), base.y + dir.y * (lo - base_along), base.z + dir.z * (lo - base_along)};
	s1 = {base.x + dir.x * (hi - base_along), base.y + dir.y * (hi - base_along), base.z + dir.z * (hi - base_along)};
	segment = true;
	return true;
}

double PointSegmentDistance(const Vertex3D &p, const Vertex3D &a, const Vertex3D &b) {
	auto ab = Sub(b, a);
	double len2 = Dot(ab, ab);
	double t = len2 > 0.0 ? std::min(1.0, std::max(0.0, Dot(Sub(p, a), ab) / len2)) : 0.0;
	return Norm(Sub(p, {a.x + ab.x * t, a.y + ab.y * t, a.z + ab.z * t}));
}

//! Distance from `p` to the boundary (every ring) of `face`, in solid-local
//! coordinates.
double BoundaryDistance(const SolidModel &model, uint32_t face, const Vertex3D &origin, const Vertex3D &p) {
	double best = INFINITY;
	for (uint32_t r = model.face_ring_offsets[face]; r < model.face_ring_offsets[face + 1]; r++) {
		uint32_t begin = model.ring_vertex_offsets[r], end = model.ring_vertex_offsets[r + 1];
		for (uint32_t i = begin; i < end; i++) {
			uint32_t j = i + 1 < end ? i + 1 : begin;
			best = std::min(best, PointSegmentDistance(p, Sub(model.vertices[model.ring_vertex_indices[i]], origin),
			                                           Sub(model.vertices[model.ring_vertex_indices[j]], origin)));
		}
	}
	return best;
}

bool BoxesOverlap(const Triangle &a, const Triangle &b, int u, int v) {
	return a.lo[u] <= b.hi[u] && b.lo[u] <= a.hi[u] && a.lo[v] <= b.hi[v] && b.lo[v] <= a.hi[v];
}

} // anonymous namespace

uint32_t CountSelfIntersections(const SolidModel &model) {
	if (model.face_triangle_offsets.size() != model.FaceCount() + 1) {
		return 0;
	}
	uint32_t count = 0;
	std::vector<Triangle> triangles;
	std::vector<uint64_t> pairs;
	for (uint32_t solid = 0; solid < model.SolidCount(); solid++) {
		uint32_t first_shell = model.solid_shell_offsets[solid];
		uint32_t face_start = model.shell_face_offsets[first_shell];
		uint32_t face_end = model.shell_face_offsets[model.solid_shell_offsets[solid + 1]];
		Vertex3D origin;
		bool have_origin = false;
		for (uint32_t shell = first_shell; shell < model.solid_shell_offsets[solid + 1] && !have_origin; shell++) {
			have_origin = ShellLocalOrigin(model, shell, origin);
		}
		if (face_start == face_end || !have_origin) {
			continue;
		}

		triangles.clear();
		double lo[3] = {INFINITY, INFINITY, INFINITY}, hi[3] = {-INFINITY, -INFINITY, -INFINITY};
		for (uint32_t f = face_start; f < face_end; f++) {
			uint32_t t_start = model.face_triangle_offsets[f];
			uint32_t t_end = model.face_triangle_offsets[f + 1];
			if (t_start == t_end) {
				continue;
			}
			auto plane = FitFacePlane(model, f, origin);
			if (!plane.defined) {
				continue;
			}
			for (uint32_t t = t_start; t < t_end; t++) {
				Triangle tri;
				for (int k = 0; k < 3; k++) {
					tri.p[k] = Sub(model.vertices[model.triangle_vertex_indices[t * 3 + k]], origin);
				}
				auto n = Cross(Sub(tri.p[1], tri.p[0]), Sub(tri.p[2], tri.p[0]));
				double n_length = Norm(n);
				if (!(n_length > 0.0)) {
					continue;
				}
				tri.own_normal = Scale(n, 1.0 / n_length);
				tri.normal = plane.normal;
				tri.offset = plane.offset;
				tri.deviation = plane.deviation;
				tri.tolerance = 2 * plane.deviation;
				tri.face = f;
				for (int axis = 0; axis < 3; axis++) {
					auto coord = [axis](const Vertex3D &p) {
						return axis == 0 ? p.x : (axis == 1 ? p.y : p.z);
					};
					tri.lo[axis] = std::min({coord(tri.p[0]), coord(tri.p[1]), coord(tri.p[2])});
					tri.hi[axis] = std::max({coord(tri.p[0]), coord(tri.p[1]), coord(tri.p[2])});
					lo[axis] = std::min(lo[axis], tri.lo[axis]);
					hi[axis] = std::max(hi[axis], tri.hi[axis]);
				}
				triangles.push_back(tri);
			}
		}
		if (triangles.size() < 2) {
			continue;
		}
		const double diagonal = std::sqrt((hi[0] - lo[0]) * (hi[0] - lo[0]) + (hi[1] - lo[1]) * (hi[1] - lo[1]) +
		                                  (hi[2] - lo[2]) * (hi[2] - lo[2]));
		const double eps = kEpsRelative * diagonal;
		const double feature = kSelfIntersectionRelative * diagonal;
		for (auto &tri : triangles) {
			tri.tolerance += feature / 2;
			for (int axis = 0; axis < 3; axis++) {
				tri.lo[axis] -= tri.tolerance;
				tri.hi[axis] += tri.tolerance;
			}
		}

		// Sweep and prune along the solid's longest axis, so a solid stretched
		// along one axis does not leave every pair in one sweep window.
		int sweep = 0;
		for (int axis = 1; axis < 3; axis++) {
			if (hi[axis] - lo[axis] > hi[sweep] - lo[sweep]) {
				sweep = axis;
			}
		}
		const int u = (sweep + 1) % 3, v = (sweep + 2) % 3;
		std::sort(triangles.begin(), triangles.end(),
		          [sweep](const Triangle &a, const Triangle &b) { return a.lo[sweep] < b.lo[sweep]; });
		pairs.clear();
		for (size_t i = 0; i < triangles.size(); i++) {
			const auto &a = triangles[i];
			for (size_t j = i + 1; j < triangles.size() && triangles[j].lo[sweep] <= a.hi[sweep]; j++) {
				const auto &b = triangles[j];
				if (a.face == b.face || !BoxesOverlap(a, b, u, v)) {
					continue;
				}
				double tolerance = a.tolerance + b.tolerance;
				bool segment;
				Vertex3D s0, s1;
				if (!TrianglesCross(a, b, tolerance, eps, segment, s0, s1)) {
					continue;
				}
				// A crossing within the tolerance of both faces' boundaries is a
				// fold of the junction where they meet (two roof faces whose
				// ridges cross by a centimetre along a sliver), which a
				// different triangulation of the same rings removes; it counts
				// only where it reaches further than that into one of them.
				bool interior = !segment;
				Vertex3D mid = Scale({s0.x + s1.x, s0.y + s1.y, s0.z + s1.z}, 0.5);
				for (const Vertex3D *p : {&s0, &mid, &s1}) {
					if (interior) {
						break;
					}
					interior = BoundaryDistance(model, a.face, origin, *p) > tolerance ||
					           BoundaryDistance(model, b.face, origin, *p) > tolerance;
				}
				if (interior) {
					uint64_t f0 = std::min(a.face, b.face), f1 = std::max(a.face, b.face);
					pairs.push_back((f0 << 32) | f1);
				}
			}
		}
		std::sort(pairs.begin(), pairs.end());
		count += static_cast<uint32_t>(std::unique(pairs.begin(), pairs.end()) - pairs.begin());
	}
	return count;
}

} // namespace duckdb_3d
