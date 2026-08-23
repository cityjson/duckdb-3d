#include "catch.hpp"
#include "kernel/geometry_math.hpp"
#include <cmath>

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

TEST_CASE("NewellRingAreaVector: a zero-area ring stays zero far from the origin", "[geometry_math]") {
	// A ring that retraces itself (p0 -> p1 -> p2 -> p1) encloses exactly zero
	// area, anywhere, at any magnitude — that is a property of the path, not of
	// the coordinates. Newell's mixed form pairs a coordinate DIFFERENCE with a
	// coordinate SUM, so on absolute coordinates the products scale as |position|
	// while the answer scales as |extent|^2; the cancellation leaves noise of
	// order n·eps·|position|·|extent|.
	//
	// That noise is what the degeneracy test in validation.cpp compares against
	// kEpsAbsolute = 1e-12, so the verdict becomes position-dependent: this ring
	// is correctly called degenerate at the origin and silently passes at RD New
	// (EPSG:28992) easting/northing magnitudes. is_valid and
	// degenerate_face_count gate ComputeVolume / ComputeSurfaceArea, so a
	// position-dependent verdict is a position-dependent hard error.
	for (double D : {0.0, 1.0e2, 8.5e4, 4.47e5, 1.0e6, 1.0e8}) {
		SolidModel model;
		model.vertices = {{D, D, 0}, {D + 1, D + 0.1, 0}, {D + 2, D + 0.2, 0}};
		model.ring_vertex_offsets = {0, 4};
		model.ring_vertex_indices = {0, 1, 2, 1};
		auto v = NewellRingAreaVector(model, 0);
		double mag = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
		INFO("offset = " << D << ", |newell| = " << mag);
		REQUIRE(mag < kEpsAbsolute);
	}
}
