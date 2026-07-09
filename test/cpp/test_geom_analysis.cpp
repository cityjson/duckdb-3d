#include "catch.hpp"
#include "kernel/geom_analysis.hpp"

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

TEST_CASE("Geom3DConvexHull: square with interior point", "[geom_analysis]") {
	GeomModel m;
	m.type = GeomType::MultiPoint;
	m.vertices = {{0, 0, 5}, {2, 0, 5}, {2, 2, 5}, {0, 2, 5}, {1, 1, 5}};
	m.part_offsets = {0, 1, 2, 3, 4, 5};
	m.ComputeBBox();
	auto hull = Geom3DConvexHull(m);
	REQUIRE(hull.type == GeomType::Polygon);
	REQUIRE(hull.vertices.size() == 4);
	REQUIRE(hull.ring_offsets == std::vector<uint32_t> {0, 4});
}

TEST_CASE("Geom3DConvexHull: collinear points become a linestring", "[geom_analysis]") {
	GeomModel m;
	m.type = GeomType::MultiPoint;
	m.vertices = {{0, 0, 1}, {1, 0, 2}, {2, 0, 3}, {3, 0, 4}};
	m.part_offsets = {0, 1, 2, 3, 4};
	m.ComputeBBox();
	auto hull = Geom3DConvexHull(m);
	REQUIRE(hull.type == GeomType::LineString);
	REQUIRE(hull.vertices.size() == 2);
	REQUIRE(hull.vertices.front().x == Approx(0.0));
	REQUIRE(hull.vertices.back().x == Approx(3.0));
	REQUIRE(hull.vertices.front().z == Approx(1.0)); // min Z
}

TEST_CASE("Geom3DConvexHull: single point", "[geom_analysis]") {
	GeomModel m;
	m.type = GeomType::Point;
	m.vertices = {{5, 5, 7}};
	m.ComputeBBox();
	auto hull = Geom3DConvexHull(m);
	REQUIRE(hull.type == GeomType::Point);
	REQUIRE(hull.vertices.size() == 1);
	REQUIRE(hull.vertices[0].x == Approx(5.0));
	REQUIRE(hull.vertices[0].z == Approx(7.0));
}
