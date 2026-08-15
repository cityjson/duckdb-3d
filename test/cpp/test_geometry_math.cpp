#include "catch.hpp"
#include "kernel/geometry_math.hpp"

using namespace duckdb_3d;

TEST_CASE("NewellRingAreaVector: CCW unit square in the XY plane", "[geometry_math]") {
	SolidModel model;
	model.vertices = {{0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0}};
	model.ring_vertex_offsets = {0, 4};
	model.ring_vertex_indices = {0, 1, 2, 3};
	auto v = NewellRingAreaVector(model, 0);
	REQUIRE(v.x == Approx(0.0));
	REQUIRE(v.y == Approx(0.0));
	REQUIRE(v.z == Approx(2.0)); // twice the ring area, +Z for CCW winding
}

TEST_CASE("NewellRingAreaVector: CW winding flips the sign", "[geometry_math]") {
	SolidModel model;
	model.vertices = {{0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0}};
	model.ring_vertex_offsets = {0, 4};
	model.ring_vertex_indices = {3, 2, 1, 0};
	auto v = NewellRingAreaVector(model, 0);
	REQUIRE(v.x == Approx(0.0));
	REQUIRE(v.y == Approx(0.0));
	REQUIRE(v.z == Approx(-2.0));
}

TEST_CASE("NewellRingAreaVector: degenerate two-vertex ring is zero", "[geometry_math]") {
	SolidModel model;
	model.vertices = {{0, 0, 0}, {1, 0, 0}};
	model.ring_vertex_offsets = {0, 2};
	model.ring_vertex_indices = {0, 1};
	auto v = NewellRingAreaVector(model, 0);
	REQUIRE(v.x == 0.0);
	REQUIRE(v.y == 0.0);
	REQUIRE(v.z == 0.0);
}
