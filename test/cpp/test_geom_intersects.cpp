#include "catch.hpp"
#include "kernel/geom_distance.hpp"
#include "kernel/geom_intersects.hpp"
#include "kernel/predicates.hpp"
#include <cmath>
#include <limits>
#include <random>
#include <vector>

using namespace duckdb_3d;

namespace {

Vertex3D V(double x, double y, double z) {
	return Vertex3D {x, y, z};
}

//! The next double above v.
double Up(double v) {
	return std::nextafter(v, std::numeric_limits<double>::infinity());
}

GeomModel Point(double x, double y, double z) {
	GeomModel m;
	m.type = GeomType::Point;
	m.vertices = {V(x, y, z)};
	m.ComputeBBox();
	return m;
}

//! An axis-aligned square in the plane z = `z`.
GeomModel Square(double x0, double y0, double z, double side) {
	GeomModel m;
	m.type = GeomType::Polygon;
	m.vertices = {V(x0, y0, z), V(x0 + side, y0, z), V(x0 + side, y0 + side, z), V(x0, y0 + side, z)};
	m.ring_offsets = {0, 4};
	m.ComputeBBox();
	return m;
}

//! The square [0, 10]^2 in z = 0 with the hole [3, 7]^2.
GeomModel SquareWithHole() {
	GeomModel m;
	m.type = GeomType::Polygon;
	m.vertices = {V(0, 0, 0), V(10, 0, 0), V(10, 10, 0), V(0, 10, 0), V(3, 3, 0), V(3, 7, 0), V(7, 7, 0), V(7, 3, 0)};
	m.ring_offsets = {0, 4, 8};
	m.ComputeBBox();
	return m;
}

GeomModel Triangle(const Vertex3D &a, const Vertex3D &b, const Vertex3D &c) {
	GeomModel m;
	m.type = GeomType::Polygon;
	m.vertices = {a, b, c};
	m.ring_offsets = {0, 3};
	m.ComputeBBox();
	return m;
}

} // namespace

TEST_CASE("PointOnSegment is exact", "[geom_intersects]") {
	// Large coordinates, where a distance computed in doubles is not exactly 0.
	const double X = 6378137.0, Y = 512345.0, Z = 103.0;
	auto a = V(X, Y, Z), b = V(X + 4, Y + 2, Z + 6);
	REQUIRE(PointOnSegment(V(X + 2, Y + 1, Z + 3), a, b));
	REQUIRE(PointOnSegment(a, a, b));
	REQUIRE(PointOnSegment(b, a, b));
	REQUIRE_FALSE(PointOnSegment(V(X + 2, Y + 1, Up(Z + 3)), a, b));
	REQUIRE_FALSE(PointOnSegment(V(X + 6, Y + 3, Z + 9), a, b)); // on the line, past b
	// A degenerate segment is its one point.
	REQUIRE(PointOnSegment(a, a, a));
	REQUIRE_FALSE(PointOnSegment(b, a, a));
}

TEST_CASE("SegmentsIntersect is exact", "[geom_intersects]") {
	// Crossing at (1, 1, 1).
	REQUIRE(SegmentsIntersect(V(0, 0, 0), V(2, 2, 2), V(2, 0, 0), V(0, 2, 2)));
	// Skew: they cross in the XY projection but one is a unit above the other.
	REQUIRE_FALSE(SegmentsIntersect(V(0, 0, 0), V(2, 2, 0), V(2, 0, 1), V(0, 2, 1)));
	// Skew by one unit in the last place.
	REQUIRE_FALSE(SegmentsIntersect(V(0, 0, 1), V(2, 2, 1), V(2, 0, Up(1.0)), V(0, 2, Up(1.0))));
	// One ends on the other (a T).
	REQUIRE(SegmentsIntersect(V(0, 0, 0), V(4, 0, 0), V(2, 0, 0), V(2, 3, 0)));
	// Collinear: overlapping, touching at an end, and apart.
	REQUIRE(SegmentsIntersect(V(0, 0, 0), V(2, 2, 2), V(1, 1, 1), V(3, 3, 3)));
	REQUIRE(SegmentsIntersect(V(0, 0, 0), V(2, 2, 2), V(2, 2, 2), V(3, 3, 3)));
	REQUIRE_FALSE(SegmentsIntersect(V(0, 0, 0), V(1, 1, 1), V(2, 2, 2), V(3, 3, 3)));
	// Parallel in one plane.
	REQUIRE_FALSE(SegmentsIntersect(V(0, 0, 0), V(2, 0, 0), V(0, 1, 0), V(2, 1, 0)));
	// Coplanar, not parallel, and the crossing point of their lines lies outside one.
	REQUIRE_FALSE(SegmentsIntersect(V(0, 0, 0), V(1, 0, 0), V(2, -1, 0), V(2, 1, 0)));
	// Degenerate segments.
	REQUIRE(SegmentsIntersect(V(1, 0, 0), V(1, 0, 0), V(0, 0, 0), V(2, 0, 0)));
	REQUIRE_FALSE(SegmentsIntersect(V(1, 1, 0), V(1, 1, 0), V(0, 0, 0), V(2, 0, 0)));
}

TEST_CASE("PointInTriangle is exact", "[geom_intersects]") {
	const double X = 84000.25, Y = 446000.5;
	// The plane z = x'/3 + 2 y'/3 through these three points holds (1, 1, 1).
	auto a = V(X, Y, 0), b = V(X + 3, Y, 1), c = V(X, Y + 3, 2);
	REQUIRE(PointInTriangle(V(X + 1, Y + 1, 1), a, b, c));
	REQUIRE_FALSE(PointInTriangle(V(X + 1, Y + 1, Up(1.0)), a, b, c));
	// On an edge and on a vertex.
	REQUIRE(PointInTriangle(V(X + 1.5, Y, 0.5), a, b, c));
	REQUIRE(PointInTriangle(b, a, b, c));
	// In the plane, outside the triangle.
	REQUIRE_FALSE(PointInTriangle(V(X + 3, Y + 3, 3), a, b, c));
	// A triangle without area is its edges.
	auto p = V(0, 0, 0), q = V(2, 2, 2), r = V(4, 4, 4);
	REQUIRE(PointInTriangle(V(3, 3, 3), p, q, r));
	REQUIRE_FALSE(PointInTriangle(V(3, 3, 2), p, q, r));
}

TEST_CASE("SegmentTriangleIntersect is exact", "[geom_intersects]") {
	// In the plane z = 1, so that one unit in the last place off it is a normal double.
	auto a = V(0, 0, 1), b = V(4, 0, 1), c = V(0, 4, 1);
	// Through the interior, through an edge, and through a vertex.
	REQUIRE(SegmentTriangleIntersect(V(1, 1, 0), V(1, 1, 2), a, b, c));
	REQUIRE(SegmentTriangleIntersect(V(2, 0, 0), V(2, 0, 2), a, b, c));
	REQUIRE(SegmentTriangleIntersect(V(4, 0, 0), V(4, 0, 2), a, b, c));
	// Past the edge by one unit in the last place.
	REQUIRE_FALSE(SegmentTriangleIntersect(V(1, Up(3.0), 0), V(1, Up(3.0), 2), a, b, c));
	// Ending on the face, and stopping one unit in the last place short of it.
	REQUIRE(SegmentTriangleIntersect(V(1, 1, 1), V(1, 1, 5), a, b, c));
	REQUIRE_FALSE(SegmentTriangleIntersect(V(1, 1, Up(1.0)), V(1, 1, 5), a, b, c));
	// In the plane: crossing an edge, wholly inside, and wholly outside.
	REQUIRE(SegmentTriangleIntersect(V(-1, 1, 1), V(1, 1, 1), a, b, c));
	REQUIRE(SegmentTriangleIntersect(V(1, 1, 1), V(1, 2, 1), a, b, c));
	REQUIRE_FALSE(SegmentTriangleIntersect(V(5, 5, 1), V(6, 5, 1), a, b, c));
	// Parallel to the plane, above it.
	REQUIRE_FALSE(SegmentTriangleIntersect(V(0, 0, 2), V(1, 1, 2), a, b, c));
}

TEST_CASE("TrianglesIntersect is exact", "[geom_intersects]") {
	auto a = V(0, 0, 0), b = V(4, 0, 0), c = V(0, 4, 0);
	// Crossing: a vertical triangle through the middle.
	REQUIRE(TrianglesIntersect(a, b, c, V(1, 1, -1), V(2, 1, 1), V(1, 2, 1)));
	// A vertex touching the face, and an edge lying across it.
	REQUIRE(TrianglesIntersect(a, b, c, V(1, 1, 0), V(2, 1, 3), V(1, 2, 3)));
	REQUIRE(TrianglesIntersect(a, b, c, V(1, -1, 0), V(1, 5, 0), V(1, 2, 3)));
	// Edge to edge.
	REQUIRE(TrianglesIntersect(a, b, c, V(2, 0, 0), V(2, -2, 0), V(2, -1, 3)));
	// Coplanar: overlapping, one inside the other, and apart.
	REQUIRE(TrianglesIntersect(a, b, c, V(1, 1, 0), V(6, 1, 0), V(1, 6, 0)));
	REQUIRE(TrianglesIntersect(a, b, c, V(0.5, 0.5, 0), V(1, 0.5, 0), V(0.5, 1, 0)));
	REQUIRE(TrianglesIntersect(V(0.5, 0.5, 0), V(1, 0.5, 0), V(0.5, 1, 0), a, b, c));
	REQUIRE_FALSE(TrianglesIntersect(a, b, c, V(3, 3, 0), V(5, 3, 0), V(3, 5, 0)));
	// Parallel planes one unit in the last place apart.
	double z = Up(1.0);
	REQUIRE(TrianglesIntersect(V(0, 0, 1), V(4, 0, 1), V(0, 4, 1), V(1, 1, 1), V(5, 1, 1), V(1, 5, 1)));
	REQUIRE_FALSE(TrianglesIntersect(V(0, 0, 1), V(4, 0, 1), V(0, 4, 1), V(1, 1, z), V(5, 1, z), V(1, 5, z)));
}

TEST_CASE("Geom3DIntersects: a gap no tolerance can see is a gap", "[geom_intersects]") {
	// Two squares 1e-12 apart: within any tolerance of 1e-9, but not touching.
	auto lower = Square(0, 0, 0, 1);
	auto upper = Square(0, 0, 1e-12, 1);
	REQUIRE(Geom3DWithin(lower, upper, 1e-9));
	REQUIRE_FALSE(Geom3DIntersects(lower, upper));
	REQUIRE(Geom3DIntersects(lower, Square(0.5, 0.5, 0, 1)));
}

TEST_CASE("Geom3DIntersects: a point on a face at large coordinates touches it", "[geom_intersects]") {
	const double X = 6378137.0, Y = 512345.0, Z = 103.0;
	auto t = Triangle(V(X, Y, Z), V(X + 3, Y, Z + 1), V(X, Y + 3, Z + 2));
	REQUIRE(Geom3DIntersects(t, Point(X + 1, Y + 1, Z + 1)));
	REQUIRE(Geom3DIntersects(Point(X + 1, Y + 1, Z + 1), t));
	REQUIRE_FALSE(Geom3DIntersects(t, Point(X + 1, Y + 1, Up(Z + 1))));
}

TEST_CASE("Geom3DIntersects: a hole is not part of the face, its edge is", "[geom_intersects]") {
	auto f = SquareWithHole();
	REQUIRE_FALSE(Geom3DIntersects(f, Point(5, 5, 0)));
	REQUIRE(Geom3DIntersects(f, Point(3, 5, 0)));
	REQUIRE(Geom3DIntersects(f, Point(1, 1, 0)));
	// A square inside the hole, and one overlapping its edge.
	REQUIRE_FALSE(Geom3DIntersects(f, Square(4, 4, 0, 2)));
	REQUIRE(Geom3DIntersects(f, Square(6, 4, 0, 2)));
}

TEST_CASE("Geom3DIntersects: lines and points", "[geom_intersects]") {
	GeomModel line;
	line.type = GeomType::LineString;
	line.vertices = {V(0, 0, 0), V(2, 2, 2), V(4, 0, 0)};
	line.ComputeBBox();
	REQUIRE(Geom3DIntersects(line, Point(3, 1, 1)));
	REQUIRE_FALSE(Geom3DIntersects(line, Point(3, 1, 1.5)));
	REQUIRE(Geom3DIntersects(line, Square(-1, -1, 1, 4)));
	REQUIRE_FALSE(Geom3DIntersects(line, Square(-1, -1, 2.5, 4)));
	REQUIRE(Geom3DIntersects(Point(1, 2, 3), Point(1, 2, 3)));
	REQUIRE_FALSE(Geom3DIntersects(Point(1, 2, 3), Point(1, 2, Up(3.0))));
}

TEST_CASE("Geom3DIntersects agrees with the distance on random triangle soups", "[geom_intersects]") {
	// Small integer coordinates make many pairs touch exactly. Whenever the two
	// disagree, the distance must be within its own rounding of 0.
	std::mt19937 rng(7);
	std::uniform_int_distribution<int> coord(0, 6);
	auto soup = [&](int n) {
		GeomModel m;
		m.type = GeomType::MultiPolygon;
		m.part_offsets.push_back(0);
		m.ring_offsets.push_back(0);
		while (static_cast<int>(m.part_offsets.size()) <= n) {
			Vertex3D v[3];
			for (auto &p : v) {
				p = V(coord(rng), coord(rng), coord(rng));
			}
			if (Collinear(v[0], v[1], v[2])) {
				continue;
			}
			for (auto &p : v) {
				m.vertices.push_back(p);
			}
			m.ring_offsets.push_back(static_cast<uint32_t>(m.vertices.size()));
			m.part_offsets.push_back(static_cast<uint32_t>(m.ring_offsets.size() - 1));
		}
		m.ComputeBBox();
		return m;
	};
	int touching = 0, apart = 0;
	for (int i = 0; i < 400; i++) {
		auto g1 = soup(1);
		auto g2 = soup(1);
		bool exact = Geom3DIntersects(g1, g2);
		double d = Geom3DDistance(g1, g2);
		if (exact) {
			touching++;
			REQUIRE(d <= 1e-12);
		} else {
			apart++;
			REQUIRE(d > 0.0);
		}
	}
	REQUIRE(touching > 20);
	REQUIRE(apart > 20);
}
