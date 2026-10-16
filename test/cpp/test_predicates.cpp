#include "catch.hpp"

#include "kernel/predicates.hpp"

#include <cmath>

using namespace duckdb_3d;

TEST_CASE("Orient3D: the sign follows the right-handed normal", "[predicates]") {
	Vertex3D a {0, 0, 0}, b {1, 0, 0}, c {0, 1, 0};
	REQUIRE(Orient3D(a, b, c, {0.2, 0.3, 1}) == 1);
	REQUIRE(Orient3D(a, b, c, {0.2, 0.3, -1}) == -1);
	REQUIRE(Orient3D(a, b, c, {5, -7, 0}) == 0);
	REQUIRE(Orient3D(b, a, c, {0.2, 0.3, 1}) == -1);
}

// Kettner et al., "Classroom examples of robustness problems in geometric
// computations": p = (0.5 + x u, 0.5 + y u) with u = 2^-53, q = (12, 12),
// r = (24, 24). The exact orientation is 12 (y - x) u, so its sign is the sign
// of y - x, and p is collinear with q and r exactly when x = y. Evaluated in
// doubles the determinant gets many of these signs wrong.
TEST_CASE("Orient2D and Orient3D: exact on Kettner's near-collinear grid", "[predicates]") {
	const double u = std::ldexp(1.0, -53);
	const Vertex3D q {12, 12, 0}, r {24, 24, 0}, apex {0, 0, 1};
	int naive_wrong = 0;
	for (int x = 0; x < 64; x++) {
		for (int y = 0; y < 64; y++) {
			const Vertex3D p {0.5 + x * u, 0.5 + y * u, 0};
			const int expected = (y > x) - (y < x);
			REQUIRE(Orient2D(p, q, r, 2) == expected);
			// p, q, r in the plane z = 0 and the apex above it: the normal of
			// (p, q, r) points up exactly when they turn counterclockwise.
			REQUIRE(Orient3D(p, q, r, apex) == expected);
			REQUIRE(Collinear(p, q, r) == (x == y));
			const double naive = (q.x - p.x) * (r.y - p.y) - (q.y - p.y) * (r.x - p.x);
			naive_wrong += ((naive > 0) - (naive < 0)) != expected;
		}
	}
	// The grid is a test of exactness only because doubles fail on it.
	REQUIRE(naive_wrong > 0);
}

TEST_CASE("Orient2D: each axis is the matching normal component", "[predicates]") {
	// (b - a) x (c - a) = (2, -3, 1) for these points.
	Vertex3D a {0, 0, 0}, b {1, 1, 1}, c {0, 1, 3};
	REQUIRE(Orient2D(a, b, c, 0) == 1);
	REQUIRE(Orient2D(a, b, c, 1) == -1);
	REQUIRE(Orient2D(a, b, c, 2) == 1);
	REQUIRE_FALSE(Collinear(a, b, c));
	REQUIRE(Collinear(a, b, {3, 3, 3}));
	REQUIRE(Collinear(a, a, c));
}
