#include "kernel/geom_intersects.hpp"
#include "kernel/geom_elements.hpp"
#include "kernel/predicates.hpp"

#include <algorithm>

namespace duckdb_3d {

namespace {

bool Same(const Vertex3D &a, const Vertex3D &b) {
	return a.x == b.x && a.y == b.y && a.z == b.z;
}

bool Between(double v, double a, double b) {
	return std::min(a, b) <= v && v <= std::max(a, b);
}

//! An axis whose projection keeps the plane through a, b, c (not collinear)
//! one-to-one: one where the triangle's projected area is not zero.
int ProjectionAxis(const Vertex3D &a, const Vertex3D &b, const Vertex3D &c, int &sign) {
	for (int axis = 2; axis >= 0; axis--) {
		sign = Orient2D(a, b, c, axis);
		if (sign != 0) {
			return axis;
		}
	}
	return -1;
}

//! Strictly opposite signs.
bool Opposite(int s, int t) {
	return (s > 0 && t < 0) || (s < 0 && t > 0);
}

} // namespace

bool PointOnSegment(const Vertex3D &p, const Vertex3D &a, const Vertex3D &b) {
	// On the line through a and b, and within the segment's box: for a point
	// on the line, the box is the segment.
	return Between(p.x, a.x, b.x) && Between(p.y, a.y, b.y) && Between(p.z, a.z, b.z) && Collinear(a, b, p);
}

bool SegmentsIntersect(const Vertex3D &p1, const Vertex3D &q1, const Vertex3D &p2, const Vertex3D &q2) {
	if (Same(p1, q1)) {
		return PointOnSegment(p1, p2, q2);
	}
	if (Same(p2, q2)) {
		return PointOnSegment(p2, p1, q1);
	}
	if (Orient3D(p1, q1, p2, q2) != 0) {
		return false;
	}
	// Coplanar. Collinear segments meet when an endpoint of one lies on the other.
	bool p2_on_line = Collinear(p1, q1, p2);
	bool q2_on_line = Collinear(p1, q1, q2);
	if (p2_on_line && q2_on_line) {
		return PointOnSegment(p2, p1, q1) || PointOnSegment(q2, p1, q1) || PointOnSegment(p1, p2, q2) ||
		       PointOnSegment(q1, p2, q2);
	}
	// Otherwise decide in a projection that keeps their plane one-to-one.
	int sign;
	int axis = p2_on_line ? ProjectionAxis(p1, q1, q2, sign) : ProjectionAxis(p1, q1, p2, sign);
	int d1 = Orient2D(p2, q2, p1, axis);
	int d2 = Orient2D(p2, q2, q1, axis);
	int d3 = Orient2D(p1, q1, p2, axis);
	int d4 = Orient2D(p1, q1, q2, axis);
	if (Opposite(d1, d2) && Opposite(d3, d4)) {
		return true;
	}
	return (d1 == 0 && PointOnSegment(p1, p2, q2)) || (d2 == 0 && PointOnSegment(q1, p2, q2)) ||
	       (d3 == 0 && PointOnSegment(p2, p1, q1)) || (d4 == 0 && PointOnSegment(q2, p1, q1));
}

bool PointInTriangle(const Vertex3D &p, const Vertex3D &a, const Vertex3D &b, const Vertex3D &c) {
	int sign;
	int axis = ProjectionAxis(a, b, c, sign);
	if (axis < 0) {
		return PointOnSegment(p, a, b) || PointOnSegment(p, b, c) || PointOnSegment(p, c, a);
	}
	if (Orient3D(a, b, c, p) != 0) {
		return false;
	}
	// In the plane: on the inner side of (or on) every edge.
	return !Opposite(Orient2D(a, b, p, axis), sign) && !Opposite(Orient2D(b, c, p, axis), sign) &&
	       !Opposite(Orient2D(c, a, p, axis), sign);
}

bool SegmentTriangleIntersect(const Vertex3D &p, const Vertex3D &q, const Vertex3D &a, const Vertex3D &b,
                              const Vertex3D &c) {
	if (Collinear(a, b, c)) {
		return SegmentsIntersect(p, q, a, b) || SegmentsIntersect(p, q, b, c) || SegmentsIntersect(p, q, c, a);
	}
	if (Same(p, q)) {
		return PointInTriangle(p, a, b, c);
	}
	int sp = Orient3D(a, b, c, p);
	int sq = Orient3D(a, b, c, q);
	if (sp == 0 && sq == 0) {
		return PointInTriangle(p, a, b, c) || PointInTriangle(q, a, b, c) || SegmentsIntersect(p, q, a, b) ||
		       SegmentsIntersect(p, q, b, c) || SegmentsIntersect(p, q, c, a);
	}
	if (sp == sq) {
		return false;
	}
	if (sp == 0) {
		return PointInTriangle(p, a, b, c);
	}
	if (sq == 0) {
		return PointInTriangle(q, a, b, c);
	}
	// The segment crosses the plane. It meets the triangle when the line through
	// it passes on the same side of (or along) each of the triangle's edges.
	int o1 = Orient3D(p, q, a, b);
	int o2 = Orient3D(p, q, b, c);
	int o3 = Orient3D(p, q, c, a);
	bool positive = o1 > 0 || o2 > 0 || o3 > 0;
	bool negative = o1 < 0 || o2 < 0 || o3 < 0;
	return !(positive && negative);
}

bool TrianglesIntersect(const Vertex3D &a1, const Vertex3D &b1, const Vertex3D &c1, const Vertex3D &a2,
                        const Vertex3D &b2, const Vertex3D &c2) {
	// All of one triangle strictly on one side of the other's plane: apart.
	auto one_side = [](const Vertex3D &a, const Vertex3D &b, const Vertex3D &c, const Vertex3D &p, const Vertex3D &q,
	                   const Vertex3D &r) {
		int sp = Orient3D(a, b, c, p);
		return sp != 0 && sp == Orient3D(a, b, c, q) && sp == Orient3D(a, b, c, r);
	};
	if (one_side(a1, b1, c1, a2, b2, c2) || one_side(a2, b2, c2, a1, b1, c1)) {
		return false;
	}
	// Two closed triangles that meet do so where an edge of one meets the other:
	// the ends of their common segment (or the boundary of their common polygon,
	// when coplanar) lie on their edges, and a triangle inside the other has its
	// edges inside it too.
	return SegmentTriangleIntersect(a1, b1, a2, b2, c2) || SegmentTriangleIntersect(b1, c1, a2, b2, c2) ||
	       SegmentTriangleIntersect(c1, a1, a2, b2, c2) || SegmentTriangleIntersect(a2, b2, a1, b1, c1) ||
	       SegmentTriangleIntersect(b2, c2, a1, b1, c1) || SegmentTriangleIntersect(c2, a2, a1, b1, c1);
}

bool BoxesOverlap(const BBox3D &a, const BBox3D &b) {
	return a.min_x <= b.max_x && b.min_x <= a.max_x && a.min_y <= b.max_y && b.min_y <= a.max_y && a.min_z <= b.max_z &&
	       b.min_z <= a.max_z;
}

namespace {

bool ElementsMeet(const Element &e1, const Element &e2) {
	const Element &a = (e1.n <= e2.n) ? e1 : e2;
	const Element &b = (e1.n <= e2.n) ? e2 : e1;
	switch (a.n * 10 + b.n) {
	case 11:
		return Same(a.v[0], b.v[0]);
	case 12:
		return PointOnSegment(a.v[0], b.v[0], b.v[1]);
	case 13:
		return PointInTriangle(a.v[0], b.v[0], b.v[1], b.v[2]);
	case 22:
		return SegmentsIntersect(a.v[0], a.v[1], b.v[0], b.v[1]);
	case 23:
		return SegmentTriangleIntersect(a.v[0], a.v[1], b.v[0], b.v[1], b.v[2]);
	default:
		return TrianglesIntersect(a.v[0], a.v[1], a.v[2], b.v[0], b.v[1], b.v[2]);
	}
}

} // namespace

bool ElementsIntersect(const std::vector<Element> &e1, const BBox3D &box1, const std::vector<Element> &e2,
                       const BBox3D &box2) {
	// Boxes are min and max of the input coordinates, so pruning on them is as
	// exact as the tests: elements whose boxes do not overlap cannot meet.
	std::vector<const Element *> candidates;
	for (const auto &e : e2) {
		if (BoxesOverlap(e.box, box1)) {
			candidates.push_back(&e);
		}
	}
	for (const auto &a : e1) {
		if (!BoxesOverlap(a.box, box2)) {
			continue;
		}
		for (const auto *b : candidates) {
			if (BoxesOverlap(a.box, b->box) && ElementsMeet(a, *b)) {
				return true;
			}
		}
	}
	return false;
}

bool Geom3DIntersects(const GeomModel &g1, const GeomModel &g2) {
	if (!BoxesOverlap(g1.bbox, g2.bbox)) {
		return false;
	}
	return ElementsIntersect(Decompose(g1), g1.bbox, Decompose(g2), g2.bbox);
}

} // namespace duckdb_3d
