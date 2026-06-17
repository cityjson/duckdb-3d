#include "catch.hpp"
#include "kernel/geom_serialize.hpp"
#include "kernel/geom_wkb_parser.hpp"

using namespace duckdb_3d;

namespace {

GeomModel Point(double x, double y, double z) {
	GeomModel m;
	m.type = GeomType::Point;
	m.vertices = {{x, y, z}};
	m.ComputeBBox();
	return m;
}

GeomModel LineString(std::initializer_list<std::array<double, 3>> pts) {
	GeomModel m;
	m.type = GeomType::LineString;
	for (const auto &p : pts) {
		m.vertices.push_back({p[0], p[1], p[2]});
	}
	m.ComputeBBox();
	return m;
}

GeomModel Polygon(std::initializer_list<std::array<double, 3>> ring) {
	GeomModel m;
	m.type = GeomType::Polygon;
	for (const auto &p : ring) {
		m.vertices.push_back({p[0], p[1], p[2]});
	}
	m.ring_offsets = {0, static_cast<uint32_t>(m.vertices.size())};
	m.ComputeBBox();
	return m;
}

} // namespace

TEST_CASE("Geom3DAsText: Point Z", "[geom_serialize]") {
	REQUIRE(Geom3DAsText(Point(1, 2, 3)) == "POINT Z (1 2 3)");
}

TEST_CASE("Geom3DAsText: LineString Z", "[geom_serialize]") {
	REQUIRE(Geom3DAsText(LineString({{0, 0, 0}, {1, 0, 0}, {1, 1, 1}})) ==
	        "LINESTRING Z (0 0 0, 1 0 0, 1 1 1)");
}

TEST_CASE("Geom3DAsText: Polygon Z repeats closing vertex", "[geom_serialize]") {
	REQUIRE(Geom3DAsText(Polygon({{0, 0, 5}, {4, 0, 5}, {4, 3, 5}, {0, 3, 5}})) ==
	        "POLYGON Z ((0 0 5, 4 0 5, 4 3 5, 0 3 5, 0 0 5))");
}

TEST_CASE("Geom3DAsGeoJSON: Point", "[geom_serialize]") {
	REQUIRE(Geom3DAsGeoJSON(Point(1, 2, 3)) == R"({"type":"Point","coordinates":[1,2,3]})");
}

TEST_CASE("Geom3DAsGeoJSON: LineString", "[geom_serialize]") {
	REQUIRE(Geom3DAsGeoJSON(LineString({{0, 0, 0}, {1, 1, 1}})) ==
	        R"({"type":"LineString","coordinates":[[0,0,0],[1,1,1]]})");
}

TEST_CASE("Geom3DAsBinary: Point Z round-trips through parser", "[geom_serialize]") {
	auto wkb = Geom3DAsBinary(Point(1, 2, 3));
	auto model = ParseGeomWKB(wkb.data(), wkb.size());
	REQUIRE(model.type == GeomType::Point);
	REQUIRE(model.vertices.size() == 1);
	REQUIRE(model.vertices[0].x == Approx(1.0));
	REQUIRE(model.vertices[0].y == Approx(2.0));
	REQUIRE(model.vertices[0].z == Approx(3.0));
}

TEST_CASE("Geom3DAsBinary: Polygon Z round-trips through parser", "[geom_serialize]") {
	auto wkb = Geom3DAsBinary(Polygon({{0, 0, 5}, {4, 0, 5}, {4, 3, 5}, {0, 3, 5}}));
	auto model = ParseGeomWKB(wkb.data(), wkb.size());
	REQUIRE(model.type == GeomType::Polygon);
	REQUIRE(model.vertices.size() == 4);
	REQUIRE(model.ring_offsets == std::vector<uint32_t>{0, 4});
}
