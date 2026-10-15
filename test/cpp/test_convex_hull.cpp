#include "catch.hpp"

#include "kernel/convex_hull.hpp"
#include "kernel/predicates.hpp"

#include <cmath>
#include <cstdint>
#include <map>
#include <random>
#include <set>
#include <utility>
#include <vector>

using namespace duckdb_3d;

namespace {

//! Polygon k's exterior ring of a PolyhedralSurface hull.
std::vector<Vertex3D> Face(const GeomModel &hull, size_t k) {
	uint32_t ring = hull.part_offsets[k];
	return {hull.vertices.begin() + hull.ring_offsets[ring], hull.vertices.begin() + hull.ring_offsets[ring + 1]};
}

size_t FaceCount(const GeomModel &hull) {
	return hull.part_offsets.size() - 1;
}

//! The properties every 3D hull must have: each face is one ring, planar and
//! convex, wound counterclockwise seen from outside, so that every input point
//! is on or behind every face; each directed edge has its reverse in exactly
//! one other face (closed, 2-manifold); V - E + F = 2; no vertex is a
//! collinear midpoint of its face's ring.
void RequireHullOf(const GeomModel &hull, const std::vector<Vertex3D> &points) {
	REQUIRE(hull.type == GeomType::PolyhedralSurface);
	std::map<std::pair<size_t, size_t>, int> edges;
	std::map<std::tuple<double, double, double>, size_t> ids;
	auto id = [&](const Vertex3D &v) {
		return ids.emplace(std::make_tuple(v.x, v.y, v.z), ids.size()).first->second;
	};
	for (size_t k = 0; k < FaceCount(hull); k++) {
		REQUIRE(hull.part_offsets[k + 1] == hull.part_offsets[k] + 1);
		auto face = Face(hull, k);
		REQUIRE(face.size() >= 3);
		for (size_t i = 0; i < face.size(); i++) {
			const auto &a = face[i];
			const auto &b = face[(i + 1) % face.size()];
			const auto &c = face[(i + 2) % face.size()];
			// Convex, counterclockwise from outside, no collinear vertex.
			REQUIRE_FALSE(Collinear(a, b, c));
			for (const auto &p : points) {
				REQUIRE(Orient3D(a, b, c, p) <= 0);
			}
			for (const auto &q : face) {
				REQUIRE(Orient3D(a, b, c, q) == 0);
			}
			edges[{id(a), id(b)}]++;
		}
	}
	for (const auto &edge : edges) {
		REQUIRE(edge.second == 1);
		REQUIRE(edges.count({edge.first.second, edge.first.first}) == 1);
	}
	const auto v = static_cast<int64_t>(ids.size());
	const auto e = static_cast<int64_t>(edges.size() / 2);
	const auto f = static_cast<int64_t>(FaceCount(hull));
	REQUIRE(v - e + f == 2);
}

std::vector<Vertex3D> Grid(int n, double step, Vertex3D origin) {
	std::vector<Vertex3D> points;
	for (int i = 0; i < n; i++) {
		for (int j = 0; j < n; j++) {
			for (int k = 0; k < n; k++) {
				points.push_back({origin.x + i * step, origin.y + j * step, origin.z + k * step});
			}
		}
	}
	return points;
}

} // namespace

TEST_CASE("ConvexHull3D: a tetrahedron is its own hull", "[convex_hull]") {
	std::vector<Vertex3D> points = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}, {0, 0, 1}, {0.1, 0.1, 0.1}};
	auto hull = ConvexHull3D(points);
	RequireHullOf(hull, points);
	REQUIRE(FaceCount(hull) == 4);
	REQUIRE(hull.vertices.size() == 12);
}

TEST_CASE("ConvexHull3D: a lattice cube has six square faces", "[convex_hull]") {
	// 125 points, 98 of them on the cube's faces and edges: the hull keeps only
	// the 8 corners, and coplanar triangles merge into one square per side.
	auto points = Grid(5, 0.25, {84910.5, 446690.25, 2.0});
	auto hull = ConvexHull3D(points);
	RequireHullOf(hull, points);
	REQUIRE(FaceCount(hull) == 6);
	for (size_t k = 0; k < 6; k++) {
		REQUIRE(Face(hull, k).size() == 4);
	}
}

TEST_CASE("ConvexHull3D: a point 2^-60 above a face is a vertex", "[convex_hull]") {
	// Exact predicates see the apex; a tolerance would fold it into the square.
	const double h = std::ldexp(1.0, -60);
	std::vector<Vertex3D> points = {{0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0}, {0, 0, -1}, {0.5, 0.5, h}};
	auto hull = ConvexHull3D(points);
	RequireHullOf(hull, points);
	bool apex = false;
	for (const auto &v : hull.vertices) {
		apex = apex || (v.x == 0.5 && v.y == 0.5 && v.z == h);
	}
	REQUIRE(apex);
}

TEST_CASE("ConvexHull3D: random clouds", "[convex_hull]") {
	std::mt19937_64 rng(20261015);
	std::uniform_real_distribution<double> uniform(-1.0, 1.0);
	for (int round = 0; round < 20; round++) {
		std::vector<Vertex3D> points;
		for (int i = 0; i < 200; i++) {
			// Points on and inside a sphere, many of them hull vertices.
			Vertex3D p {uniform(rng), uniform(rng), uniform(rng)};
			double len = std::sqrt(p.x * p.x + p.y * p.y + p.z * p.z);
			double r = (i % 2 == 0) ? 1.0 / len : 0.5;
			points.push_back({p.x * r + 85000, p.y * r + 446000, p.z * r});
		}
		RequireHullOf(ConvexHull3D(points), points);
	}
}

TEST_CASE("ConvexHull3D: coplanar points give a polygon", "[convex_hull]") {
	// A tilted square (plane z = x) with interior and edge points.
	std::vector<Vertex3D> points = {{0, 0, 0}, {2, 0, 2}, {2, 2, 2}, {0, 2, 0}, {1, 1, 1}, {1, 0, 1}, {2, 1, 2}};
	auto hull = ConvexHull3D(points);
	REQUIRE(hull.type == GeomType::Polygon);
	REQUIRE(hull.ring_offsets == std::vector<uint32_t> {0, 4});
	for (size_t i = 0; i < 4; i++) {
		REQUIRE_FALSE(Collinear(hull.vertices[i], hull.vertices[(i + 1) % 4], hull.vertices[(i + 2) % 4]));
	}
}

TEST_CASE("ConvexHull3D: collinear points give their extreme segment", "[convex_hull]") {
	std::vector<Vertex3D> points = {{1, 0, 2}, {3, 0, 4}, {0, 0, 1}, {2, 0, 3}};
	auto hull = ConvexHull3D(points);
	REQUIRE(hull.type == GeomType::LineString);
	REQUIRE(hull.vertices.size() == 2);
	REQUIRE(hull.vertices[0] == Vertex3D {0, 0, 1});
	REQUIRE(hull.vertices[1] == Vertex3D {3, 0, 4});
}

TEST_CASE("ConvexHull3D: one distinct point gives a point; none gives an empty point", "[convex_hull]") {
	auto hull = ConvexHull3D({{5, 5, 7}, {5, 5, 7}});
	REQUIRE(hull.type == GeomType::Point);
	REQUIRE(hull.vertices.size() == 1);
	REQUIRE(hull.vertices[0] == Vertex3D {5, 5, 7});
	auto empty = ConvexHull3D({});
	REQUIRE(empty.type == GeomType::Point);
	REQUIRE(empty.vertices.empty());
}
