#include "catch.hpp"
#include "kernel/geom_construct.hpp"
#include <array>
#include <stdexcept>
#include <vector>

using namespace duckdb_3d;

namespace {

//! Build an axis-aligned square Polygon GEOM_3D (CCW) in the z=0 plane.
GeomModel SquarePolygon(double side) {
	GeomModel m;
	m.type = GeomType::Polygon;
	m.vertices = {{0, 0, 0}, {side, 0, 0}, {side, side, 0}, {0, side, 0}};
	m.ring_offsets = {0, 4};
	m.ComputeBBox();
	return m;
}

//! Build a PolyhedralSurface GEOM_3D from a list of triangular faces (each given as
//! three vertices, no closing duplicate).
GeomModel SurfaceFromTriangles(const std::vector<std::array<Vertex3D, 3>> &faces) {
	GeomModel m;
	m.type = GeomType::PolyhedralSurface;
	m.ring_offsets.push_back(0);
	m.part_offsets.push_back(0);
	for (const auto &f : faces) {
		for (const auto &v : f) {
			m.vertices.push_back(v);
		}
		m.ring_offsets.push_back(static_cast<uint32_t>(m.vertices.size()));
		m.part_offsets.push_back(static_cast<uint32_t>(m.ring_offsets.size() - 1));
	}
	m.ComputeBBox();
	return m;
}

//! The four faces of a closed, outward-oriented unit tetrahedron.
std::vector<std::array<Vertex3D, 3>> ClosedTetraFaces() {
	return {{{{0, 0, 0}, {0, 1, 0}, {1, 0, 0}}},
	        {{{0, 0, 0}, {1, 0, 0}, {0, 0, 1}}},
	        {{{1, 0, 0}, {0, 1, 0}, {0, 0, 1}}},
	        {{{0, 0, 0}, {0, 0, 1}, {0, 1, 0}}}};
}

} // namespace

TEST_CASE("BuildSolidFromSurface: closed tetrahedron becomes a valid solid",
          "[geom_construct]") {
	auto solid = BuildSolidFromSurface(SurfaceFromTriangles(ClosedTetraFaces()));

	REQUIRE(solid.SolidCount() == 1);
	REQUIRE(solid.FaceCount() == 4);
	REQUIRE(solid.vertices.size() == 4); // deduped tetra corners
	REQUIRE(solid.validation.is_closed);
	REQUIRE(solid.validation.is_manifold);
	REQUIRE(solid.validation.is_oriented);
}

TEST_CASE("BuildSolidFromSurface: open surface raises", "[geom_construct]") {
	auto faces = ClosedTetraFaces();
	faces.pop_back(); // drop the closing face → open shell
	REQUIRE_THROWS_AS(BuildSolidFromSurface(SurfaceFromTriangles(faces)), std::runtime_error);
}

TEST_CASE("BuildSolidFromSurface: non-surface input raises", "[geom_construct]") {
	GeomModel poly;
	poly.type = GeomType::Polygon;
	poly.vertices = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}};
	poly.ring_offsets = {0, 3};
	poly.ComputeBBox();
	REQUIRE_THROWS_AS(BuildSolidFromSurface(poly), std::runtime_error);
}

TEST_CASE("BuildExtrudedSolid: unit square extruded into a closed box", "[geom_construct]") {
	auto solid = BuildExtrudedSolid(SquarePolygon(1.0), 2.0);

	REQUIRE(solid.SolidCount() == 1);
	REQUIRE(solid.ShellCount() == 1);
	// 1 bottom + 1 top + 4 sides.
	REQUIRE(solid.FaceCount() == 6);
	// 4 bottom + 4 top corners after dedup.
	REQUIRE(solid.vertices.size() == 8);
	REQUIRE(solid.validation.is_closed);
	REQUIRE(solid.validation.is_manifold);
	REQUIRE(solid.validation.is_oriented);
	// Box spans z in [0, 2].
	REQUIRE(solid.bbox.min_z == 0.0);
	REQUIRE(solid.bbox.max_z == 2.0);
}

TEST_CASE("BuildExtrudedSolid: clockwise footprint still yields an oriented box",
          "[geom_construct]") {
	GeomModel cw;
	cw.type = GeomType::Polygon;
	// Clockwise winding (negative shoelace area).
	cw.vertices = {{0, 0, 0}, {0, 1, 0}, {1, 1, 0}, {1, 0, 0}};
	cw.ring_offsets = {0, 4};
	cw.ComputeBBox();

	auto solid = BuildExtrudedSolid(cw, 3.0);
	REQUIRE(solid.validation.is_closed);
	REQUIRE(solid.validation.is_oriented);
	REQUIRE(solid.bbox.max_z == 3.0);
}

TEST_CASE("BuildExtrudedSolid: rejects non-positive height", "[geom_construct]") {
	REQUIRE_THROWS_AS(BuildExtrudedSolid(SquarePolygon(1.0), 0.0), std::runtime_error);
	REQUIRE_THROWS_AS(BuildExtrudedSolid(SquarePolygon(1.0), -1.0), std::runtime_error);
}

TEST_CASE("BuildExtrudedSolid: rejects non-polygon input", "[geom_construct]") {
	GeomModel pt;
	pt.type = GeomType::Point;
	pt.vertices = {{0, 0, 0}};
	pt.ComputeBBox();
	REQUIRE_THROWS_AS(BuildExtrudedSolid(pt, 1.0), std::runtime_error);
}
