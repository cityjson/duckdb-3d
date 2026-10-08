#include "catch.hpp"
#include "kernel/affine.hpp"
#include <cmath>
#include <limits>
#include <stdexcept>
#include <vector>

using namespace duckdb_3d;

namespace {

void RequireVertex(const Vertex3D &v, double x, double y, double z) {
	REQUIRE(v.x == Approx(x).margin(1e-12));
	REQUIRE(v.y == Approx(y).margin(1e-12));
	REQUIRE(v.z == Approx(z).margin(1e-12));
}

} // namespace

TEST_CASE("Affine: a row-major 4x4 maps a point as M * (x, y, z, 1)", "[affine]") {
	// Rotation by 90 degrees about Z, uniform scale 2, translation (1, 2, 3):
	//   x' = 0x - 2y + 0z + 1,  y' = 2x + 0y + 0z + 2,  z' = 0x + 0y + 2z + 3.
	auto a = AffineFromMatrix4x4({0, -2, 0, 1, 2, 0, 0, 2, 0, 0, 2, 3, 0, 0, 0, 1});
	RequireVertex(ApplyAffine(a, {1, 0, 0}), 1, 4, 3);
	RequireVertex(ApplyAffine(a, {0, 1, 0}), -1, 2, 3);
	RequireVertex(ApplyAffine(a, {0, 0, 1}), 1, 2, 5);
	REQUIRE(LinearDeterminant(a) == Approx(8.0));
}

TEST_CASE("Affine: translating after the matrix adds a reference point", "[affine]") {
	// An implicit geometry's relative vertex v lands at M * v + p.
	auto a = ThenTranslate(AffineFromMatrix4x4({0, -2, 0, 1, 2, 0, 0, 2, 0, 0, 2, 3, 0, 0, 0, 1}), {10, 20, 30});
	RequireVertex(ApplyAffine(a, {1, 0, 0}), 11, 24, 33);

	std::vector<Vertex3D> vertices = {{0, 0, 0}, {1, 1, 1}};
	ApplyAffine(a, vertices);
	RequireVertex(vertices[0], 11, 22, 33);
	RequireVertex(vertices[1], 9, 24, 35);
}

TEST_CASE("Affine: the identity leaves points where they are", "[affine]") {
	RequireVertex(ApplyAffine(AffineIdentity(), {1.5, -2.5, 3.25}), 1.5, -2.5, 3.25);
	REQUIRE(LinearDeterminant(AffineIdentity()) == 1.0);
}

TEST_CASE("Affine: a reflection has a negative determinant", "[affine]") {
	auto mirror = AffineFromMatrix4x4({-1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1});
	REQUIRE(LinearDeterminant(mirror) == Approx(-1.0));
}

TEST_CASE("Affine: a matrix that is not a 4x4 affine transform is rejected", "[affine]") {
	SECTION("wrong length") {
		REQUIRE_THROWS_WITH(AffineFromMatrix4x4({1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0}), Catch::Contains("16"));
	}
	SECTION("projective last row") {
		REQUIRE_THROWS_WITH(AffineFromMatrix4x4({1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 1, 1}),
		                    Catch::Contains("0, 0, 0, 1"));
	}
	SECTION("non-finite entry") {
		double nan = std::numeric_limits<double>::quiet_NaN();
		REQUIRE_THROWS_WITH(AffineFromMatrix4x4({1, 0, 0, nan, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1}),
		                    Catch::Contains("finite"));
	}
}

TEST_CASE("Affine: singularity is judged relative to the matrix's scale", "[affine]") {
	// An oblique projection onto z = 0: (x, y, z) -> (x - z, y - z, 0). Every
	// solid it maps collapses to zero volume.
	REQUIRE(IsSingularLinear(AffineFromMatrix4x4({1, 0, -1, 0, 0, 1, -1, 0, 0, 0, 0, 0, 0, 0, 0, 1})));
	// Dropping Z outright.
	REQUIRE(IsSingularLinear(AffineFromMatrix4x4({1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1})));
	// Rows (1, 0, 0), (0, 1, 0), (1, 1, 1e-15): det 1e-15 against row norms of
	// order 1 — numerically a projection.
	REQUIRE(IsSingularLinear(AffineFromMatrix4x4({1, 0, 0, 0, 0, 1, 0, 0, 1, 1, 1e-15, 0, 0, 0, 0, 1})));
	// A tiny uniform scale is well-conditioned: det 1e-18, but so is the product
	// of the row norms, so it is not singular. Nor is a mirror.
	REQUIRE_FALSE(IsSingularLinear(AffineFromMatrix4x4({1e-6, 0, 0, 0, 0, 1e-6, 0, 0, 0, 0, 1e-6, 0, 0, 0, 0, 1})));
	REQUIRE_FALSE(IsSingularLinear(AffineFromMatrix4x4({-1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1})));
	REQUIRE_FALSE(IsSingularLinear(AffineIdentity()));
}
