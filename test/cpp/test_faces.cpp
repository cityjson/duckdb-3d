#include "catch.hpp"
#include "kernel/faces.hpp"
#include "kernel/geom_construct.hpp"
#include "kernel/geom_wkb_parser.hpp"
#include "kernel/measurements.hpp"
#include "kernel/triangulation.hpp"
#include "kernel/validation.hpp"

#include <cmath>
#include <cstring>
#include <vector>

using namespace duckdb_3d;

namespace {

GeomModel Polygon(std::vector<std::vector<Vertex3D>> rings) {
	GeomModel m;
	m.type = GeomType::Polygon;
	m.ring_offsets.push_back(0);
	for (auto &ring : rings) {
		m.vertices.insert(m.vertices.end(), ring.begin(), ring.end());
		m.ring_offsets.push_back(static_cast<uint32_t>(m.vertices.size()));
	}
	m.ComputeBBox();
	TriangulateGeomModel(m);
	return m;
}

SolidModel Box() {
	GeomModel square = Polygon({{{0, 0, 0}, {2, 0, 0}, {2, 2, 0}, {0, 2, 0}}});
	return BuildExtrudedSolid(square, 3.0);
}

//! A solid model of loose faces, each a list of rings, triangulated: enough for
//! per-face measures, which do not need a closed shell.
SolidModel LooseFaces(const std::vector<std::vector<std::vector<Vertex3D>>> &faces) {
	SolidModel m;
	m.solid_shell_offsets = {0, 1};
	m.shell_face_offsets = {0, static_cast<uint32_t>(faces.size())};
	m.face_ring_offsets = {0};
	m.ring_vertex_offsets = {0};
	for (auto &face : faces) {
		for (auto &ring : face) {
			for (auto &v : ring) {
				m.ring_vertex_indices.push_back(static_cast<uint32_t>(m.vertices.size()));
				m.vertices.push_back(v);
			}
			m.ring_vertex_offsets.push_back(static_cast<uint32_t>(m.ring_vertex_indices.size()));
		}
		m.face_ring_offsets.push_back(static_cast<uint32_t>(m.ring_vertex_offsets.size() - 1));
	}
	m.ComputeBBox();
	TriangulateSolidModel(m);
	return m;
}

} // namespace

TEST_CASE("Faces: a box's faces sum to its surface area and face out of it", "[faces]") {
	auto box = Box();
	auto faces = SolidFaceMeasures(box);
	REQUIRE(faces.size() == box.FaceCount());
	double total = 0;
	Vertex3D normal_sum = {0, 0, 0};
	for (auto &f : faces) {
		REQUIRE(f.has_area);
		REQUIRE(f.has_normal);
		total += f.area;
		REQUIRE(std::sqrt(f.normal.x * f.normal.x + f.normal.y * f.normal.y + f.normal.z * f.normal.z) == Approx(1.0));
		// Each normal points away from the box's centre (1, 1, 1.5).
		auto polygon = SolidFacePolygon(box, static_cast<uint32_t>(&f - faces.data()));
		const auto &p = polygon.vertices[0];
		REQUIRE((p.x - 1) * f.normal.x + (p.y - 1) * f.normal.y + (p.z - 1.5) * f.normal.z > 0);
		normal_sum.x += f.normal.x * f.area;
		normal_sum.y += f.normal.y * f.area;
		normal_sum.z += f.normal.z * f.area;
	}
	REQUIRE(total == ComputeSurfaceArea(box));
	// A closed surface's vector area is zero.
	REQUIRE(normal_sum.x == Approx(0.0).margin(1e-12));
	REQUIRE(normal_sum.y == Approx(0.0).margin(1e-12));
	REQUIRE(normal_sum.z == Approx(0.0).margin(1e-12));
}

TEST_CASE("Faces: a face of a solid is a Polygon in the solid's CRS", "[faces]") {
	auto box = Box();
	box.crs = "EPSG:28992";
	auto polygon = SolidFacePolygon(box, 0);
	REQUIRE(polygon.type == GeomType::Polygon);
	REQUIRE(polygon.crs == "EPSG:28992");
	REQUIRE(polygon.PolygonCount() == 1);
	REQUIRE(polygon.vertices.size() == box.ring_vertex_offsets[1] - box.ring_vertex_offsets[0]);
	REQUIRE(polygon.bbox.max_z >= polygon.bbox.min_z);
}

TEST_CASE("Faces: a polygon's hole is left out of its area", "[faces]") {
	auto holed =
	    Polygon({{{0, 0, 0}, {10, 0, 0}, {10, 10, 0}, {0, 10, 0}}, {{3, 3, 0}, {3, 7, 0}, {7, 7, 0}, {7, 3, 0}}});
	auto faces = GeomFaceMeasures(holed);
	REQUIRE(faces.size() == 1);
	REQUIRE(faces[0].area == Approx(84.0));
	REQUIRE(faces[0].normal.z == 1.0);
	REQUIRE(faces[0].normal.x == 0.0);
	REQUIRE(faces[0].normal.y == 0.0);
}

TEST_CASE("Faces: a solid face's hole is left out of its area, whichever way it is wound", "[faces]") {
	const std::vector<Vertex3D> outer = {{0, 0, 0}, {10, 0, 0}, {10, 10, 0}, {0, 10, 0}};
	const std::vector<Vertex3D> against = {{3, 3, 0}, {3, 7, 0}, {7, 7, 0}, {7, 3, 0}};
	const std::vector<Vertex3D> along = {{3, 3, 0}, {7, 3, 0}, {7, 7, 0}, {3, 7, 0}};
	for (auto &hole : {against, along}) {
		auto faces = SolidFaceMeasures(LooseFaces({{outer, hole}}));
		REQUIRE(faces.size() == 1);
		REQUIRE(faces[0].has_area);
		REQUIRE(faces[0].area == Approx(84.0));
		REQUIRE(faces[0].normal.z == 1.0);
	}
}

TEST_CASE("Faces: a warped face measures its triangles, as the same polygon does", "[faces]") {
	// One corner lifted by 1: the vector area of the ring is 4, its triangles more.
	auto solid = LooseFaces({{{{0, 0, 0}, {2, 0, 0}, {2, 2, 1}, {0, 2, 0}}}});
	auto faces = SolidFaceMeasures(solid);
	auto as_polygon = GeomFaceMeasures(SolidFacePolygon(solid, 0));
	REQUIRE(faces[0].area > 4.0 + 1e-3);
	REQUIRE(faces[0].area == Approx(as_polygon[0].area).epsilon(1e-12));
}

TEST_CASE("Faces: a solid's surface area is its faces' areas, warped faces included", "[faces]") {
	auto box = Box();
	// Lift one top corner, which warps the top and two walls.
	for (auto &v : box.vertices) {
		if (v.x == 2 && v.y == 2 && v.z == 3) {
			v.z = 4;
		}
	}
	TriangulateSolidModel(box);
	ValidateSolidModel(box);
	double total = 0;
	for (auto &f : SolidFaceMeasures(box)) {
		REQUIRE(f.has_area);
		total += f.area;
	}
	REQUIRE(ComputeSurfaceArea(box) == Approx(total).epsilon(1e-12));
}

TEST_CASE("Faces: the normal follows the exterior ring alone", "[faces]") {
	// A tilted exterior with a hole off its plane: the hole does not lean the normal.
	auto faces = SolidFaceMeasures(
	    LooseFaces({{{{0, 0, 0}, {4, 0, 0}, {4, 4, 4}, {0, 4, 4}}, {{1, 1, 1}, {1, 2, 2.5}, {2, 2, 2}, {2, 1, 1}}}}));
	REQUIRE(faces[0].normal.x == 0.0);
	REQUIRE(faces[0].normal.y == Approx(-std::sqrt(0.5)));
	REQUIRE(faces[0].normal.z == Approx(std::sqrt(0.5)));
}

TEST_CASE("Faces: a solid face without triangles has no area and no normal", "[faces]") {
	// Collinear, so zero area; and a bow-tie, which the triangulator cannot tile.
	auto faces = SolidFaceMeasures(
	    LooseFaces({{{{5, 5, 5}, {6, 6, 6}, {7, 7, 7}}}, {{{0, 0, 0}, {2, 2, 0}, {2, 0, 0}, {0, 2, 0}}}}));
	REQUIRE(faces.size() == 2);
	for (auto &f : faces) {
		REQUIRE_FALSE(f.has_area);
		REQUIRE_FALSE(f.has_normal);
	}
}

TEST_CASE("Faces: a horizontal face far from the origin leans exactly nowhere", "[faces]") {
	// At RD New magnitudes a Newell vector on absolute coordinates picks up a
	// horizontal component from rounding; referenced locally it has none.
	auto flat = Polygon({{{84995.31, 446838.27, 4.73}, {84999.17, 446838.91, 4.73}, {84998.02, 446842.66, 4.73}}});
	auto faces = GeomFaceMeasures(flat);
	REQUIRE(faces[0].normal.x == 0.0);
	REQUIRE(faces[0].normal.y == 0.0);
	double azimuth = -1;
	REQUIRE_FALSE(FaceAzimuthDegrees(faces[0].normal, azimuth));
	REQUIRE(azimuth == -1);
	REQUIRE(FaceSlopeDegrees(faces[0].normal) == 0.0);
}

TEST_CASE("Faces: slope and azimuth follow from the normal", "[faces]") {
	const double r = std::sqrt(0.5);
	double azimuth = 0;
	// Leaning east and up at 45 degrees.
	REQUIRE(FaceSlopeDegrees({r, 0, r}) == Approx(45.0));
	REQUIRE(FaceAzimuthDegrees({r, 0, r}, azimuth));
	REQUIRE(azimuth == Approx(90.0));
	// Facing down is no steeper than facing up.
	REQUIRE(FaceSlopeDegrees({r, 0, -r}) == Approx(45.0));
	REQUIRE(FaceSlopeDegrees({0, 0, -1}) == 0.0);
	REQUIRE(FaceSlopeDegrees({0, -1, 0}) == 90.0);
	REQUIRE(FaceAzimuthDegrees({0, 1, 0}, azimuth));
	REQUIRE(azimuth == 0.0);
	REQUIRE(FaceAzimuthDegrees({0, -1, 0}, azimuth));
	REQUIRE(azimuth == 180.0);
	REQUIRE(FaceAzimuthDegrees({-1, 0, 0}, azimuth));
	REQUIRE(azimuth == 270.0);
	// North-west by a hair is just under 360, never 360 itself.
	REQUIRE(FaceAzimuthDegrees({-1e-300, 1, 0}, azimuth));
	REQUIRE(azimuth < 360.0);
	REQUIRE(azimuth >= 0.0);
}

TEST_CASE("Faces: a polygon without triangles has no area and no normal", "[faces]") {
	auto sliver = Polygon({{{0, 0, 0}, {1, 1, 1}, {2, 2, 2}}});
	auto faces = GeomFaceMeasures(sliver);
	REQUIRE(faces.size() == 1);
	REQUIRE_FALSE(faces[0].has_area);
	REQUIRE_FALSE(faces[0].has_normal);
}

TEST_CASE("Faces: points and lines have no faces", "[faces]") {
	GeomModel line;
	line.type = GeomType::LineString;
	line.vertices = {{0, 0, 0}, {1, 1, 1}};
	REQUIRE(GeomFaceMeasures(line).empty());
}

TEST_CASE("Shells: a shell is a PolyhedralSurface of the solid's faces", "[faces]") {
	auto box = Box();
	box.crs = "EPSG:7415";
	auto shell = SolidShellSurface(box, 0);
	REQUIRE(shell.type == GeomType::PolyhedralSurface);
	REQUIRE(shell.crs == "EPSG:7415");
	REQUIRE(shell.PolygonCount() == box.FaceCount());
	REQUIRE(shell.bbox.max_z == 3.0);
	auto faces = GeomFaceMeasures(shell);
	double total = 0;
	for (auto &f : faces) {
		total += f.area;
	}
	REQUIRE(total == Approx(ComputeSurfaceArea(box)));
}

TEST_CASE("Shells: a solid of a collection stands on its own", "[faces]") {
	// Two boxes side by side in one model: the second is the first moved 5 in x.
	auto a = Box();
	SolidModel both = a;
	const uint32_t base = static_cast<uint32_t>(both.vertices.size());
	for (auto v : a.vertices) {
		both.vertices.push_back({v.x + 5, v.y, v.z});
	}
	const uint32_t ring_base = static_cast<uint32_t>(both.ring_vertex_indices.size());
	for (auto i : a.ring_vertex_indices) {
		both.ring_vertex_indices.push_back(i + base);
	}
	for (size_t r = 1; r < a.ring_vertex_offsets.size(); r++) {
		both.ring_vertex_offsets.push_back(a.ring_vertex_offsets[r] + ring_base);
	}
	const uint32_t face_ring_base = a.RingCount();
	for (size_t f = 1; f < a.face_ring_offsets.size(); f++) {
		both.face_ring_offsets.push_back(a.face_ring_offsets[f] + face_ring_base);
	}
	both.shell_face_offsets.push_back(2 * a.FaceCount());
	both.solid_shell_offsets.push_back(2);
	both.crs = "EPSG:28992";
	both.ComputeBBox();
	TriangulateSolidModel(both);
	REQUIRE(both.SolidCount() == 2);

	auto second = SolidPart(both, 1);
	REQUIRE(second.SolidCount() == 1);
	REQUIRE(second.ShellCount() == 1);
	REQUIRE(second.FaceCount() == a.FaceCount());
	REQUIRE(second.vertices.size() == a.vertices.size());
	REQUIRE(second.crs == "EPSG:28992");
	REQUIRE(second.bbox.min_x == 5.0);
	REQUIRE(second.validation.is_valid);
	REQUIRE(ComputeVolume(second) == Approx(12.0));
}

TEST_CASE("Elements: a GEOM_3D's elements are its parts or patches", "[faces]") {
	GeomModel multi;
	multi.type = GeomType::MultiPoint;
	multi.vertices = {{1, 2, 3}, {4, 5, 6}};
	multi.part_offsets = {0, 1, 2};
	multi.crs = "EPSG:4979";
	GeomModel out;
	REQUIRE(GeomElement(multi, 1, out));
	REQUIRE(out.type == GeomType::Point);
	REQUIRE(out.vertices.size() == 1);
	REQUIRE(out.vertices[0].x == 4.0);
	REQUIRE(out.crs == "EPSG:4979");
	REQUIRE_FALSE(GeomElement(multi, 2, out));

	auto holed =
	    Polygon({{{0, 0, 0}, {10, 0, 0}, {10, 10, 0}, {0, 10, 0}}, {{3, 3, 0}, {3, 7, 0}, {7, 7, 0}, {7, 3, 0}}});
	GeomModel surface;
	surface.type = GeomType::PolyhedralSurface;
	surface.vertices = holed.vertices;
	surface.ring_offsets = holed.ring_offsets;
	surface.part_offsets = {0, 2};
	surface.ComputeBBox();
	REQUIRE(GeomElement(surface, 0, out));
	REQUIRE(out.type == GeomType::Polygon);
	REQUIRE(out.ring_offsets == std::vector<uint32_t> {0, 4, 8});
	REQUIRE(out.part_offsets.empty());
	REQUIRE(out.polygon_triangle_offsets.empty());

	GeomModel line;
	line.type = GeomType::LineString;
	line.vertices = {{0, 0, 0}, {1, 1, 1}};
	REQUIRE(GeomElement(line, 0, out));
	REQUIRE(out.type == GeomType::LineString);
	REQUIRE_FALSE(GeomElement(line, 1, out));
}
