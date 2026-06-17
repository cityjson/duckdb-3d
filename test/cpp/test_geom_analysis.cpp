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
