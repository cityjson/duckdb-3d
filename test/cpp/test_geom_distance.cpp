#include "catch.hpp"
#include "kernel/geom_distance.hpp"
#include <array>
#include <cmath>
#include <limits>
#include <random>
#include <vector>

using namespace duckdb_3d;

namespace {
constexpr double kEps = 1e-9;
Vertex3D V(double x, double y, double z) {
	return Vertex3D {x, y, z};
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
	m.vertices = {V(x0, y0, z), V(x0 + side, y0, z), V(x0 + side, y0 + side, z), V(x0, y0 + side, z)};
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
	REQUIRE(DistSegmentSegment(V(0, 0, 0), V(10, 0, 0), V(0, 5, 0), V(10, 5, 0)) == Approx(5.0).epsilon(kEps));
}

TEST_CASE("DistSegmentSegment: skew segments crossing in XY but offset in Z", "[geom_distance]") {
	// seg1 along X at z=0, seg2 along Y at z=3, crossing over (5,0).
	REQUIRE(DistSegmentSegment(V(0, 0, 0), V(10, 0, 0), V(5, -5, 3), V(5, 5, 3)) == Approx(3.0).epsilon(kEps));
}

TEST_CASE("DistSegmentSegment: intersecting segments give zero", "[geom_distance]") {
	REQUIRE(DistSegmentSegment(V(0, 0, 0), V(10, 0, 0), V(5, -5, 0), V(5, 5, 0)) == Approx(0.0).margin(kEps));
}

TEST_CASE("DistSegmentSegment: clamps to endpoints", "[geom_distance]") {
	// Two colinear-X segments separated along X: [0,2] and [5,7] → gap 3.
	REQUIRE(DistSegmentSegment(V(0, 0, 0), V(2, 0, 0), V(5, 0, 0), V(7, 0, 0)) == Approx(3.0).epsilon(kEps));
}

TEST_CASE("DistPointTriangle: point above interior", "[geom_distance]") {
	// Triangle in z=0 plane; point hovering over an interior point.
	REQUIRE(DistPointTriangle(V(1, 1, 5), V(0, 0, 0), V(4, 0, 0), V(0, 4, 0)) == Approx(5.0).epsilon(kEps));
}

TEST_CASE("DistPointTriangle: point in plane inside triangle is zero", "[geom_distance]") {
	REQUIRE(DistPointTriangle(V(1, 1, 0), V(0, 0, 0), V(4, 0, 0), V(0, 4, 0)) == Approx(0.0).margin(kEps));
}

TEST_CASE("DistPointTriangle: closest to a vertex", "[geom_distance]") {
	// Point beyond the (0,0,0) corner.
	REQUIRE(DistPointTriangle(V(-3, -4, 0), V(0, 0, 0), V(4, 0, 0), V(0, 4, 0)) == Approx(5.0).epsilon(kEps));
}

TEST_CASE("DistPointTriangle: closest to an edge", "[geom_distance]") {
	// Point outside the x-edge, in plane: edge from (0,0,0)-(4,0,0); point (2,-3,0).
	REQUIRE(DistPointTriangle(V(2, -3, 0), V(0, 0, 0), V(4, 0, 0), V(0, 4, 0)) == Approx(3.0).epsilon(kEps));
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
	REQUIRE(DistTriangleTriangle(V(0, 0, 0), V(4, 0, 0), V(0, 4, 0), V(0, 0, 5), V(4, 0, 5), V(0, 4, 5)) ==
	        Approx(5.0).epsilon(kEps));
}

TEST_CASE("DistTriangleTriangle: intersecting triangles give zero", "[geom_distance]") {
	// Second triangle stands vertically through the first (in z=0 plane).
	REQUIRE(DistTriangleTriangle(V(0, 0, 0), V(4, 0, 0), V(0, 4, 0), V(1, 1, -1), V(2, 1, -1), V(1, 1, 2)) ==
	        Approx(0.0).margin(kEps));
}

TEST_CASE("DistTriangleTriangle: separated in plane", "[geom_distance]") {
	// Two coplanar triangles separated by a gap of 2 along X.
	REQUIRE(DistTriangleTriangle(V(0, 0, 0), V(1, 0, 0), V(0, 1, 0), V(3, 0, 0), V(4, 0, 0), V(3, 1, 0)) ==
	        Approx(2.0).epsilon(kEps));
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
	REQUIRE(Geom3DDistance(Square(0, 0, 0, 4), Square(10, 0, 0, 4)) == Approx(6.0).epsilon(kEps));
}

TEST_CASE("Geom3DDistance: point inside a polygon footprint is zero", "[geom_distance]") {
	REQUIRE(Geom3DDistance(Point(2, 2, 0), Square(0, 0, 0, 4)) == Approx(0.0).margin(kEps));
}

TEST_CASE("Geom3DClosestPoints: two points", "[geom_distance]") {
	auto pair = Geom3DClosestPoints(Point(0, 0, 0), Point(3, 4, 0));
	REQUIRE(pair.p.x == Approx(0.0));
	REQUIRE(pair.q.x == Approx(3.0));
}

TEST_CASE("Geom3DClosestPoints: point above a polygon", "[geom_distance]") {
	auto pair = Geom3DClosestPoints(Point(2, 2, 5), Square(0, 0, 0, 4));
	REQUIRE(pair.p.z == Approx(5.0));
	REQUIRE(pair.q.x == Approx(2.0));
	REQUIRE(pair.q.y == Approx(2.0));
	REQUIRE(pair.q.z == Approx(0.0));
}

TEST_CASE("Geom3DClosestPoints: intersecting geometries coincide", "[geom_distance]") {
	auto pair = Geom3DClosestPoints(Point(2, 1.5, 0), Square(0, 0, 0, 4));
	REQUIRE(DistPointPoint(pair.p, pair.q) == Approx(0.0).margin(kEps));
}

TEST_CASE("Geom3DMaxDistance: point to point equals the min distance", "[geom_distance]") {
	REQUIRE(Geom3DMaxDistance(Point(0, 0, 0), Point(3, 4, 0)) == Approx(5.0).epsilon(kEps));
}

TEST_CASE("Geom3DMaxDistance: point to the farthest polygon vertex", "[geom_distance]") {
	// Square corners (0,0,0)..(4,4,0); farthest from origin is (4,4,0) → sqrt(32).
	REQUIRE(Geom3DMaxDistance(Point(0, 0, 0), Square(0, 0, 0, 4)) == Approx(std::sqrt(32.0)).epsilon(kEps));
}

TEST_CASE("Geom3DMaxDistance: two squares, farthest corner pair", "[geom_distance]") {
	// A: x[0,4], B: x[10,14]; farthest corner pair spans dx=14, dy=4 → sqrt(212).
	REQUIRE(Geom3DMaxDistance(Square(0, 0, 0, 4), Square(10, 0, 0, 4)) == Approx(std::sqrt(212.0)).epsilon(kEps));
}

TEST_CASE("Geom3DBBoxDistance: overlapping bboxes give zero", "[geom_distance]") {
	// A point inside a square's bbox footprint: the boxes overlap in XY and Z.
	REQUIRE(Geom3DBBoxDistance(Point(1, 1, 0), Square(0, 0, 0, 4)) == Approx(0.0));
}

TEST_CASE("Geom3DBBoxDistance: axis-separated boxes give the gap", "[geom_distance]") {
	// Square spans x,y in [0,4]; point at x=10 → gap of 6 along x only.
	REQUIRE(Geom3DBBoxDistance(Point(10, 2, 0), Square(0, 0, 0, 4)) == Approx(6.0).epsilon(kEps));
}

TEST_CASE("Geom3DBBoxDistance: never exceeds the true distance (valid lower bound)", "[geom_distance]") {
	auto a = Point(10, 10, 10);
	auto b = Square(0, 0, 0, 4);
	REQUIRE(Geom3DBBoxDistance(a, b) <= Geom3DDistance(a, b) + kEps);
}

TEST_CASE("Geom3DWithin: agrees with Geom3DDistance threshold", "[geom_distance]") {
	auto a = Point(10, 2, 0);
	auto b = Square(0, 0, 0, 4);
	double d = Geom3DDistance(a, b); // exact: 6.0
	REQUIRE(Geom3DWithin(a, b, d + 0.5) == true);
	REQUIRE(Geom3DWithin(a, b, d - 0.5) == false);
}

TEST_CASE("Geom3DWithin: bbox-separated pair is rejected", "[geom_distance]") {
	// Far apart: the bbox lower bound alone exceeds the threshold.
	REQUIRE(Geom3DWithin(Point(1000, 1000, 1000), Square(0, 0, 0, 4), 5.0) == false);
}

TEST_CASE("Geom3DWithin: touching geometries are within any non-negative threshold", "[geom_distance]") {
	REQUIRE(Geom3DWithin(Point(2, 2, 0), Square(0, 0, 0, 4), 0.0) == true);
}

TEST_CASE("BBoxDistance computes the gap between boxes", "[geom_distance]") {
	BBox3D a {0, 0, 0, 1, 1, 1};
	BBox3D b {4, 0, 0, 5, 1, 1}; // 3 apart along x only
	REQUIRE(BBoxDistance(a, b) == Approx(3.0));

	BBox3D c {3, 4, 0, 4, 5, 1}; // gaps: 2 along x, 3 along y, 0 along z
	REQUIRE(BBoxDistance(a, c) == Approx(std::sqrt(2.0 * 2.0 + 3.0 * 3.0)));

	BBox3D d {0.5, 0.5, 0.5, 2, 2, 2}; // overlapping
	REQUIRE(BBoxDistance(a, d) == 0.0);
}

namespace {

//! A PolyhedralSurface of `faces` random triangles and quads inside a `size`-wide
//! cube at `origin`, at projected-CRS magnitudes. Seeded, so every run sees the
//! same surface.
GeomModel RandomSurface(uint32_t seed, Vertex3D origin, double size, int faces) {
	std::mt19937 rng(seed);
	std::uniform_real_distribution<double> unit(0.0, 1.0);
	GeomModel m;
	m.type = GeomType::PolyhedralSurface;
	m.part_offsets.push_back(0);
	m.ring_offsets.push_back(0);
	for (int f = 0; f < faces; f++) {
		int n = (f % 3 == 0) ? 4 : 3;
		for (int k = 0; k < n; k++) {
			m.vertices.push_back(
			    V(origin.x + size * unit(rng), origin.y + size * unit(rng), origin.z + size * unit(rng)));
		}
		m.ring_offsets.push_back(static_cast<uint32_t>(m.vertices.size()));
		m.part_offsets.push_back(static_cast<uint32_t>(m.ring_offsets.size() - 1));
	}
	m.ComputeBBox();
	return m;
}

//! The surface's elements as distance sees them: each face's exterior ring,
//! fan-triangulated from its first vertex.
std::vector<std::array<Vertex3D, 3>> FanTriangles(const GeomModel &m) {
	std::vector<std::array<Vertex3D, 3>> out;
	for (size_t k = 0; k + 1 < m.part_offsets.size(); k++) {
		uint32_t begin = m.ring_offsets[m.part_offsets[k]];
		uint32_t end = m.ring_offsets[m.part_offsets[k] + 1];
		for (uint32_t i = begin + 1; i + 1 < end; i++) {
			out.push_back({m.vertices[begin], m.vertices[i], m.vertices[i + 1]});
		}
	}
	return out;
}

//! Every element pair, in order: the minimum distance, and the first pair that
//! attains it.
struct BruteForce {
	double distance;
	ClosestPointPair pair;
};

BruteForce BruteForceDistance(const GeomModel &g1, const GeomModel &g2) {
	auto t1 = FanTriangles(g1);
	auto t2 = FanTriangles(g2);
	BruteForce best {std::numeric_limits<double>::infinity(), {}};
	for (const auto &a : t1) {
		for (const auto &b : t2) {
			auto pair = ClosestPointPairTriangleTriangle(a[0], a[1], a[2], b[0], b[1], b[2]);
			double d = DistPointPoint(pair.p, pair.q);
			if (d < best.distance) {
				best = {d, pair};
			}
		}
	}
	return best;
}

} // namespace

TEST_CASE("Geom3DDistance and friends match the brute-force sweep over every element pair", "[geom_distance]") {
	// Separated, nearly touching, and interpenetrating surface pairs, each with
	// enough faces that most element pairs are far apart.
	const Vertex3D base {84000.0, 446000.0, 0.0};
	struct Case {
		Vertex3D offset;
		double size;
	};
	const Case cases[] = {
	    {{30.0, 0.0, 0.0}, 10.0}, {{10.5, 3.0, 1.0}, 10.0}, {{4.0, 4.0, 0.0}, 10.0}, {{0.0, 25.0, 8.0}, 6.0}};
	uint32_t seed = 1;
	for (const auto &c : cases) {
		auto g1 = RandomSurface(seed++, base, 10.0, 60);
		auto g2 = RandomSurface(seed++, {base.x + c.offset.x, base.y + c.offset.y, base.z + c.offset.z}, c.size, 60);
		auto brute = BruteForceDistance(g1, g2);

		REQUIRE(Geom3DDistance(g1, g2) == brute.distance);

		auto pair = Geom3DClosestPoints(g1, g2);
		REQUIRE(pair.p.x == brute.pair.p.x);
		REQUIRE(pair.p.y == brute.pair.p.y);
		REQUIRE(pair.p.z == brute.pair.p.z);
		REQUIRE(pair.q.x == brute.pair.q.x);
		REQUIRE(pair.q.y == brute.pair.q.y);
		REQUIRE(pair.q.z == brute.pair.q.z);

		for (double t : {0.0, 0.5 * brute.distance, brute.distance, 1.5 * brute.distance + 0.1, 5.0}) {
			REQUIRE(Geom3DWithin(g1, g2, t) == (brute.distance <= t));
		}
	}
}
