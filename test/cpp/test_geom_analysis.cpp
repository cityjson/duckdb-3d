#include "catch.hpp"
#include "kernel/geom_analysis.hpp"
#include <cmath>

using namespace duckdb_3d;

namespace {

GeomModel FlatSquare() {
	GeomModel m;
	m.type = GeomType::Polygon;
	m.vertices = {{0, 0, 0}, {2, 0, 0}, {2, 2, 0}, {0, 2, 0}};
	m.ring_offsets = {0, 4};
	m.ComputeBBox();
	return m;
}

//! A quad whose four corners are NOT coplanar (one corner lifted in Z).
GeomModel WarpedQuad() {
	GeomModel m;
	m.type = GeomType::Polygon;
	m.vertices = {{0, 0, 0}, {2, 0, 0}, {2, 2, 5}, {0, 2, 0}};
	m.ring_offsets = {0, 4};
	m.ComputeBBox();
	return m;
}

} // namespace

TEST_CASE("Geom3DIsPlanar: flat polygon is planar", "[geom_analysis]") {
	REQUIRE(Geom3DIsPlanar(FlatSquare()));
}

TEST_CASE("Geom3DIsPlanar: warped quad is not planar", "[geom_analysis]") {
	REQUIRE_FALSE(Geom3DIsPlanar(WarpedQuad()));
}

TEST_CASE("Geom3DIsPlanar: a tilted but flat polygon is planar", "[geom_analysis]") {
	GeomModel m;
	m.type = GeomType::Polygon;
	// Lies in the plane z = x (all points satisfy it) → planar.
	m.vertices = {{0, 0, 0}, {2, 0, 2}, {2, 2, 2}, {0, 2, 0}};
	m.ring_offsets = {0, 4};
	m.ComputeBBox();
	REQUIRE(Geom3DIsPlanar(m));
}

TEST_CASE("Geom3DIsPlanar: a point is trivially planar", "[geom_analysis]") {
	GeomModel m;
	m.type = GeomType::Point;
	m.vertices = {{1, 2, 3}};
	m.ComputeBBox();
	REQUIRE(Geom3DIsPlanar(m));
}

TEST_CASE("Geom3DCentroid: point is itself", "[geom_analysis]") {
	GeomModel m;
	m.type = GeomType::Point;
	m.vertices = {{1, 2, 3}};
	m.ComputeBBox();
	auto c = Geom3DCentroid(m);
	REQUIRE(c.x == Approx(1.0));
	REQUIRE(c.y == Approx(2.0));
	REQUIRE(c.z == Approx(3.0));
}

TEST_CASE("Geom3DCentroid: single segment midpoint", "[geom_analysis]") {
	GeomModel m;
	m.type = GeomType::LineString;
	m.vertices = {{0, 0, 0}, {4, 0, 0}};
	m.ComputeBBox();
	auto c = Geom3DCentroid(m);
	REQUIRE(c.x == Approx(2.0));
	REQUIRE(c.y == Approx(0.0));
	REQUIRE(c.z == Approx(0.0));
}

TEST_CASE("Geom3DCentroid: two-segment polyline is length-weighted", "[geom_analysis]") {
	GeomModel m;
	m.type = GeomType::LineString;
	m.vertices = {{0, 0, 0}, {4, 0, 0}, {4, 3, 0}};
	m.ComputeBBox();
	auto c = Geom3DCentroid(m);
	// Segment lengths 4 and 3; midpoints (2,0,0) and (4,1.5,0).
	REQUIRE(c.x == Approx((4 * 2.0 + 3 * 4.0) / 7.0));
	REQUIRE(c.y == Approx((4 * 0.0 + 3 * 1.5) / 7.0));
	REQUIRE(c.z == Approx(0.0));
}

TEST_CASE("Geom3DCentroid: axis-aligned square", "[geom_analysis]") {
	GeomModel m;
	m.type = GeomType::Polygon;
	m.vertices = {{0, 0, 0}, {4, 0, 0}, {4, 4, 0}, {0, 4, 0}};
	m.ring_offsets = {0, 4};
	m.ComputeBBox();
	auto c = Geom3DCentroid(m);
	REQUIRE(c.x == Approx(2.0));
	REQUIRE(c.y == Approx(2.0));
	REQUIRE(c.z == Approx(0.0));
}

TEST_CASE("Geom3DCentroid: tilted square preserves plane", "[geom_analysis]") {
	GeomModel m;
	m.type = GeomType::Polygon;
	// Square in the plane z = x.
	m.vertices = {{0, 0, 0}, {2, 0, 2}, {2, 2, 2}, {0, 2, 0}};
	m.ring_offsets = {0, 4};
	m.ComputeBBox();
	auto c = Geom3DCentroid(m);
	REQUIRE(c.x == Approx(1.0));
	REQUIRE(c.y == Approx(1.0));
	REQUIRE(c.z == Approx(1.0));
}

TEST_CASE("Geom3DCentroid: right triangle", "[geom_analysis]") {
	GeomModel m;
	m.type = GeomType::Polygon;
	m.vertices = {{0, 0, 0}, {6, 0, 0}, {0, 6, 0}};
	m.ring_offsets = {0, 3};
	m.ComputeBBox();
	auto c = Geom3DCentroid(m);
	REQUIRE(c.x == Approx(2.0));
	REQUIRE(c.y == Approx(2.0));
	REQUIRE(c.z == Approx(0.0));
}

TEST_CASE("Geom3DFootprintHull: square with interior point", "[geom_analysis]") {
	GeomModel m;
	m.type = GeomType::MultiPoint;
	m.vertices = {{0, 0, 5}, {2, 0, 5}, {2, 2, 5}, {0, 2, 5}, {1, 1, 5}};
	m.part_offsets = {0, 1, 2, 3, 4, 5};
	m.ComputeBBox();
	auto hull = Geom3DFootprintHull(m);
	REQUIRE(hull.type == GeomType::Polygon);
	REQUIRE(hull.vertices.size() == 4);
	REQUIRE(hull.ring_offsets == std::vector<uint32_t> {0, 4});
}

TEST_CASE("Geom3DFootprintHull: collinear points become a linestring", "[geom_analysis]") {
	GeomModel m;
	m.type = GeomType::MultiPoint;
	m.vertices = {{0, 0, 1}, {1, 0, 2}, {2, 0, 3}, {3, 0, 4}};
	m.part_offsets = {0, 1, 2, 3, 4};
	m.ComputeBBox();
	auto hull = Geom3DFootprintHull(m);
	REQUIRE(hull.type == GeomType::LineString);
	REQUIRE(hull.vertices.size() == 2);
	REQUIRE(hull.vertices.front().x == Approx(0.0));
	REQUIRE(hull.vertices.back().x == Approx(3.0));
	REQUIRE(hull.vertices.front().z == Approx(1.0)); // min Z
}

TEST_CASE("Geom3DFootprintHull: single point", "[geom_analysis]") {
	GeomModel m;
	m.type = GeomType::Point;
	m.vertices = {{5, 5, 7}};
	m.ComputeBBox();
	auto hull = Geom3DFootprintHull(m);
	REQUIRE(hull.type == GeomType::Point);
	REQUIRE(hull.vertices.size() == 1);
	REQUIRE(hull.vertices[0].x == Approx(5.0));
	REQUIRE(hull.vertices[0].z == Approx(7.0));
}

TEST_CASE("Geom3DFootprintArea: axis-aligned square", "[geom_analysis]") {
	REQUIRE(Geom3DFootprintArea(FlatSquare()) == Approx(4.0)); // 2 x 2
}

TEST_CASE("Geom3DFootprintArea: right triangle", "[geom_analysis]") {
	GeomModel m;
	m.type = GeomType::Polygon;
	m.vertices = {{0, 0, 0}, {6, 0, 0}, {0, 6, 0}};
	m.ring_offsets = {0, 3};
	m.ComputeBBox();
	REQUIRE(Geom3DFootprintArea(m) == Approx(18.0)); // 1/2 x 6 x 6
}

TEST_CASE("Geom3DFootprintArea: orientation-independent (clockwise ring)", "[geom_analysis]") {
	// Same square wound clockwise — the footprint magnitude is unchanged.
	GeomModel m;
	m.type = GeomType::Polygon;
	m.vertices = {{0, 0, 0}, {0, 2, 0}, {2, 2, 0}, {2, 0, 0}};
	m.ring_offsets = {0, 4};
	m.ComputeBBox();
	REQUIRE(Geom3DFootprintArea(m) == Approx(4.0));
}

TEST_CASE("Geom3DFootprintArea: uses the XY projection, not the 3D area", "[geom_analysis]") {
	// Square in the plane z = x: its true 3D area is 2 * (2 * sqrt(2)) ≈ 5.657,
	// but the XY footprint is a 2 x 2 square = 4.
	GeomModel m;
	m.type = GeomType::Polygon;
	m.vertices = {{0, 0, 0}, {2, 0, 2}, {2, 2, 2}, {0, 2, 0}};
	m.ring_offsets = {0, 4};
	m.ComputeBBox();
	REQUIRE(Geom3DFootprintArea(m) == Approx(4.0));
}

TEST_CASE("Geom3DFootprintArea: a vertical wall projects to zero", "[geom_analysis]") {
	// A polygon in the x-z plane (all y = 0): its XY projection is a line.
	GeomModel m;
	m.type = GeomType::Polygon;
	m.vertices = {{0, 0, 0}, {2, 0, 0}, {2, 0, 3}, {0, 0, 3}};
	m.ring_offsets = {0, 4};
	m.ComputeBBox();
	REQUIRE(Geom3DFootprintArea(m) == Approx(0.0));
}

TEST_CASE("Geom3DFootprintArea: polygon with a hole subtracts the hole", "[geom_analysis]") {
	// 4x4 outer square with a 2x2 hole → 16 - 4 = 12. Hole wound opposite.
	GeomModel m;
	m.type = GeomType::Polygon;
	m.vertices = {{0, 0, 0}, {4, 0, 0}, {4, 4, 0}, {0, 4, 0}, {1, 1, 0}, {1, 3, 0}, {3, 3, 0}, {3, 1, 0}};
	m.ring_offsets = {0, 4, 8};
	m.ComputeBBox();
	REQUIRE(Geom3DFootprintArea(m) == Approx(12.0));
}

TEST_CASE("Geom3DFootprintArea: a closed PolyhedralSurface shell is halved", "[geom_analysis]") {
	// Unit cube as a 6-face PolyhedralSurface. The top and bottom each project to
	// a unit square (area 1); the four walls project to zero. A two-sided shell is
	// crossed twice per vertical column, so the footprint is 0.5 * (1 + 1) = 1.0,
	// NOT the naive face-sum of 2.0.
	GeomModel m;
	m.type = GeomType::PolyhedralSurface;
	m.vertices = {
	    {0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0}, // bottom (z=0)
	    {0, 0, 1}, {1, 0, 1}, {1, 1, 1}, {0, 1, 1}, // top    (z=1)
	    {0, 0, 0}, {1, 0, 0}, {1, 0, 1}, {0, 0, 1}, // wall y=0
	    {1, 0, 0}, {1, 1, 0}, {1, 1, 1}, {1, 0, 1}, // wall x=1
	    {1, 1, 0}, {0, 1, 0}, {0, 1, 1}, {1, 1, 1}, // wall y=1
	    {0, 1, 0}, {0, 0, 0}, {0, 0, 1}, {0, 1, 1}, // wall x=0
	};
	m.ring_offsets = {0, 4, 8, 12, 16, 20, 24};
	m.part_offsets = {0, 1, 2, 3, 4, 5, 6};
	m.ComputeBBox();
	REQUIRE(Geom3DFootprintArea(m) == Approx(1.0));
}

TEST_CASE("Geom3DFootprintArea: non-areal geometries are zero", "[geom_analysis]") {
	GeomModel pt;
	pt.type = GeomType::Point;
	pt.vertices = {{1, 2, 3}};
	pt.ComputeBBox();
	REQUIRE(Geom3DFootprintArea(pt) == Approx(0.0));

	GeomModel line;
	line.type = GeomType::LineString;
	line.vertices = {{0, 0, 0}, {4, 0, 0}, {4, 3, 0}};
	line.ComputeBBox();
	REQUIRE(Geom3DFootprintArea(line) == Approx(0.0));
}

namespace {

//! The square [0, 10]^2 in z = 0 with the hole [3, 7]^2.
GeomModel SquareWithHole() {
	GeomModel m;
	m.type = GeomType::Polygon;
	m.vertices = {{0, 0, 0}, {10, 0, 0}, {10, 10, 0}, {0, 10, 0}, {3, 3, 0}, {3, 7, 0}, {7, 7, 0}, {7, 3, 0}};
	m.ring_offsets = {0, 4, 8};
	m.ComputeBBox();
	return m;
}

//! Two faces of the cube [0, 1]^3, the bottom and the front, as a PolyhedralSurface.
GeomModel TwoFaces() {
	GeomModel m;
	m.type = GeomType::PolyhedralSurface;
	m.vertices = {{0, 0, 0}, {0, 1, 0}, {1, 1, 0}, {1, 0, 0}, {0, 0, 0}, {1, 0, 0}, {1, 0, 1}, {0, 0, 1}};
	m.ring_offsets = {0, 4, 8};
	m.part_offsets = {0, 1, 2};
	m.ComputeBBox();
	return m;
}

} // namespace

TEST_CASE("Geom3DArea: polygons, holes excluded, and surfaces", "[geom_analysis]") {
	REQUIRE(Geom3DArea(FlatSquare()) == Approx(4.0));
	REQUIRE(Geom3DArea(SquareWithHole()) == Approx(84.0));
	REQUIRE(Geom3DArea(TwoFaces()) == Approx(2.0));
	// A tilted square: side 2 along x, 2 * sqrt(2) along the slope y = z.
	GeomModel tilted;
	tilted.type = GeomType::Polygon;
	tilted.vertices = {{0, 0, 0}, {2, 0, 0}, {2, 2, 2}, {0, 2, 2}};
	tilted.ring_offsets = {0, 4};
	tilted.ComputeBBox();
	REQUIRE(Geom3DArea(tilted) == Approx(4.0 * std::sqrt(2.0)));
	GeomModel line;
	line.type = GeomType::LineString;
	line.vertices = {{0, 0, 0}, {1, 1, 1}};
	line.ComputeBBox();
	REQUIRE(Geom3DArea(line) == 0.0);
}

TEST_CASE("Geom3DPerimeter: a polygon's rings, a surface's boundary", "[geom_analysis]") {
	REQUIRE(Geom3DPerimeter(FlatSquare()) == Approx(8.0));
	REQUIRE(Geom3DPerimeter(SquareWithHole()) == Approx(56.0));
	// The shared edge is inside the surface, not on its boundary.
	REQUIRE(Geom3DPerimeter(TwoFaces()) == Approx(6.0));
	// A MultiPolygon's members are separate polygons: every ring counts.
	GeomModel multi = TwoFaces();
	multi.type = GeomType::MultiPolygon;
	REQUIRE(Geom3DPerimeter(multi) == Approx(8.0));
	// A closed surface has no boundary.
	GeomModel cube;
	cube.type = GeomType::PolyhedralSurface;
	cube.vertices = {{0, 0, 0}, {0, 1, 0}, {1, 1, 0}, {1, 0, 0}, {0, 0, 1}, {1, 0, 1}, {1, 1, 1}, {0, 1, 1},
	                 {0, 0, 0}, {1, 0, 0}, {1, 0, 1}, {0, 0, 1}, {1, 0, 0}, {1, 1, 0}, {1, 1, 1}, {1, 0, 1},
	                 {1, 1, 0}, {0, 1, 0}, {0, 1, 1}, {1, 1, 1}, {0, 1, 0}, {0, 0, 0}, {0, 0, 1}, {0, 1, 1}};
	cube.ring_offsets = {0, 4, 8, 12, 16, 20, 24};
	cube.part_offsets = {0, 1, 2, 3, 4, 5, 6};
	cube.ComputeBBox();
	REQUIRE(Geom3DPerimeter(cube) == 0.0);
	GeomModel line;
	line.type = GeomType::LineString;
	line.vertices = {{0, 0, 0}, {1, 1, 1}};
	line.ComputeBBox();
	REQUIRE(Geom3DPerimeter(line) == 0.0);
}
