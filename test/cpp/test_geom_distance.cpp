#include "catch.hpp"
#include "kernel/geom_distance.hpp"
#include <cmath>

using namespace duckdb_3d;

namespace {
constexpr double kEps = 1e-9;
Vertex3D V(double x, double y, double z) {
	return Vertex3D{x, y, z};
}

//! Build a Point GeomModel.
GeomModel Point(double x, double y, double z) {
	GeomModel m;
	m.type = GeomType::Point;
	m.vertices = {V(x, y, z)};
	m.ComputeBBox();
	return m;
}

//! Build an axis-aligned square Polygon in the z=`z` plane, lower corner (x0,y0),
//! side length `side`.
GeomModel Square(double x0, double y0, double z, double side) {
	GeomModel m;
	m.type = GeomType::Polygon;
	m.vertices = {V(x0, y0, z), V(x0 + side, y0, z), V(x0 + side, y0 + side, z),
	              V(x0, y0 + side, z)};
	m.ring_offsets = {0, 4};
	m.ComputeBBox();
	return m;
}
} // namespace

TEST_CASE("DistPointPoint", "[geom_distance]") {
	REQUIRE(DistPointPoint(V(0, 0, 0), V(3, 4, 12)) == Approx(13.0).epsilon(kEps));
	REQUIRE(DistPointPoint(V(1, 1, 1), V(1, 1, 1)) == Approx(0.0));
}

TEST_CASE("DistPointSegment: projection falls inside", "[geom_distance]") {
	// Segment along X axis from (0,0,0) to (10,0,0); point above its midpoint.
	REQUIRE(DistPointSegment(V(5, 4, 0), V(0, 0, 0), V(10, 0, 0)) == Approx(4.0).epsilon(kEps));
}

TEST_CASE("DistPointSegment: projection clamps to endpoint", "[geom_distance]") {
	// Point beyond the far endpoint clamps to (10,0,0).
	REQUIRE(DistPointSegment(V(13, 0, 0), V(0, 0, 0), V(10, 0, 0)) == Approx(3.0).epsilon(kEps));
}

TEST_CASE("DistPointSegment: degenerate segment is a point", "[geom_distance]") {
	REQUIRE(DistPointSegment(V(0, 0, 0), V(3, 0, 4), V(3, 0, 4)) == Approx(5.0).epsilon(kEps));
}

TEST_CASE("DistSegmentSegment: parallel offset segments", "[geom_distance]") {
	REQUIRE(DistSegmentSegment(V(0, 0, 0), V(10, 0, 0), V(0, 5, 0), V(10, 5, 0)) ==
	        Approx(5.0).epsilon(kEps));
}

TEST_CASE("DistSegmentSegment: skew segments crossing in XY but offset in Z", "[geom_distance]") {
	// seg1 along X at z=0, seg2 along Y at z=3, crossing over (5,0).
	REQUIRE(DistSegmentSegment(V(0, 0, 0), V(10, 0, 0), V(5, -5, 3), V(5, 5, 3)) ==
	        Approx(3.0).epsilon(kEps));
}

TEST_CASE("DistSegmentSegment: intersecting segments give zero", "[geom_distance]") {
	REQUIRE(DistSegmentSegment(V(0, 0, 0), V(10, 0, 0), V(5, -5, 0), V(5, 5, 0)) ==
	        Approx(0.0).margin(kEps));
}

TEST_CASE("DistSegmentSegment: clamps to endpoints", "[geom_distance]") {
	// Two colinear-X segments separated along X: [0,2] and [5,7] → gap 3.
	REQUIRE(DistSegmentSegment(V(0, 0, 0), V(2, 0, 0), V(5, 0, 0), V(7, 0, 0)) ==
	        Approx(3.0).epsilon(kEps));
}

TEST_CASE("DistPointTriangle: point above interior", "[geom_distance]") {
	// Triangle in z=0 plane; point hovering over an interior point.
	REQUIRE(DistPointTriangle(V(1, 1, 5), V(0, 0, 0), V(4, 0, 0), V(0, 4, 0)) ==
	        Approx(5.0).epsilon(kEps));
}

TEST_CASE("DistPointTriangle: point in plane inside triangle is zero", "[geom_distance]") {
	REQUIRE(DistPointTriangle(V(1, 1, 0), V(0, 0, 0), V(4, 0, 0), V(0, 4, 0)) ==
	        Approx(0.0).margin(kEps));
}

TEST_CASE("DistPointTriangle: closest to a vertex", "[geom_distance]") {
	// Point beyond the (0,0,0) corner.
	REQUIRE(DistPointTriangle(V(-3, -4, 0), V(0, 0, 0), V(4, 0, 0), V(0, 4, 0)) ==
	        Approx(5.0).epsilon(kEps));
}

TEST_CASE("DistPointTriangle: closest to an edge", "[geom_distance]") {
	// Point outside the x-edge, in plane: edge from (0,0,0)-(4,0,0); point (2,-3,0).
	REQUIRE(DistPointTriangle(V(2, -3, 0), V(0, 0, 0), V(4, 0, 0), V(0, 4, 0)) ==
	        Approx(3.0).epsilon(kEps));
}

TEST_CASE("DistSegmentTriangle: segment pierces triangle", "[geom_distance]") {
	REQUIRE(DistSegmentTriangle(V(1, 1, -2), V(1, 1, 2), V(0, 0, 0), V(4, 0, 0), V(0, 4, 0)) ==
	        Approx(0.0).margin(kEps));
}

TEST_CASE("DistSegmentTriangle: segment hovering above interior", "[geom_distance]") {
	REQUIRE(DistSegmentTriangle(V(1, 1, 5), V(3, 1, 5), V(0, 0, 0), V(4, 0, 0), V(0, 4, 0)) ==
	        Approx(5.0).epsilon(kEps));
}

TEST_CASE("DistSegmentTriangle: segment beside an edge", "[geom_distance]") {
	// Segment parallel to the x-edge but 3 units away in -y, in plane.
	REQUIRE(DistSegmentTriangle(V(1, -3, 0), V(3, -3, 0), V(0, 0, 0), V(4, 0, 0), V(0, 4, 0)) ==
	        Approx(3.0).epsilon(kEps));
}

TEST_CASE("DistTriangleTriangle: parallel triangles offset in Z", "[geom_distance]") {
	REQUIRE(DistTriangleTriangle(V(0, 0, 0), V(4, 0, 0), V(0, 4, 0), V(0, 0, 5), V(4, 0, 5),
	                             V(0, 4, 5)) == Approx(5.0).epsilon(kEps));
}

TEST_CASE("DistTriangleTriangle: intersecting triangles give zero", "[geom_distance]") {
	// Second triangle stands vertically through the first (in z=0 plane).
	REQUIRE(DistTriangleTriangle(V(0, 0, 0), V(4, 0, 0), V(0, 4, 0), V(1, 1, -1), V(2, 1, -1),
	                             V(1, 1, 2)) == Approx(0.0).margin(kEps));
}

TEST_CASE("DistTriangleTriangle: separated in plane", "[geom_distance]") {
	// Two coplanar triangles separated by a gap of 2 along X.
	REQUIRE(DistTriangleTriangle(V(0, 0, 0), V(1, 0, 0), V(0, 1, 0), V(3, 0, 0), V(4, 0, 0),
	                             V(3, 1, 0)) == Approx(2.0).epsilon(kEps));
}

TEST_CASE("Geom3DDistance: point to point", "[geom_distance]") {
	REQUIRE(Geom3DDistance(Point(0, 0, 0), Point(3, 4, 0)) == Approx(5.0).epsilon(kEps));
}

TEST_CASE("Geom3DDistance: point above a polygon", "[geom_distance]") {
	// Point hovering 5 above the interior of a 4x4 square.
	REQUIRE(Geom3DDistance(Point(2, 2, 5), Square(0, 0, 0, 4)) == Approx(5.0).epsilon(kEps));
	// Symmetric.
	REQUIRE(Geom3DDistance(Square(0, 0, 0, 4), Point(2, 2, 5)) == Approx(5.0).epsilon(kEps));
}

TEST_CASE("Geom3DDistance: two coplanar polygons with a gap", "[geom_distance]") {
	// Square A spans x[0,4], square B spans x[10,14]; gap = 6.
	REQUIRE(Geom3DDistance(Square(0, 0, 0, 4), Square(10, 0, 0, 4)) ==
	        Approx(6.0).epsilon(kEps));
}

TEST_CASE("Geom3DDistance: point inside a polygon footprint is zero", "[geom_distance]") {
	REQUIRE(Geom3DDistance(Point(2, 2, 0), Square(0, 0, 0, 4)) == Approx(0.0).margin(kEps));
}

TEST_CASE("Geom3DMaxDistance: point to point equals the min distance", "[geom_distance]") {
	REQUIRE(Geom3DMaxDistance(Point(0, 0, 0), Point(3, 4, 0)) == Approx(5.0).epsilon(kEps));
}

TEST_CASE("Geom3DMaxDistance: point to the farthest polygon vertex", "[geom_distance]") {
	// Square corners (0,0,0)..(4,4,0); farthest from origin is (4,4,0) → sqrt(32).
	REQUIRE(Geom3DMaxDistance(Point(0, 0, 0), Square(0, 0, 0, 4)) ==
	        Approx(std::sqrt(32.0)).epsilon(kEps));
}

TEST_CASE("Geom3DMaxDistance: two squares, farthest corner pair", "[geom_distance]") {
	// A: x[0,4], B: x[10,14]; farthest corner pair spans dx=14, dy=4 → sqrt(212).
	REQUIRE(Geom3DMaxDistance(Square(0, 0, 0, 4), Square(10, 0, 0, 4)) ==
	        Approx(std::sqrt(212.0)).epsilon(kEps));
}
