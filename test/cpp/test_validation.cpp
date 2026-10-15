#include "catch.hpp"
#include "kernel/validation.hpp"
#include "kernel/measurements.hpp"
#include "kernel/model_builder.hpp"
#include "kernel/wkb_parser.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>

using namespace duckdb_3d;

namespace {

class WKBBuilder {
public:
	std::vector<uint8_t> buffer;
	void u8(uint8_t v) {
		buffer.push_back(v);
	}
	void u32(uint32_t v) {
		buffer.push_back(v & 0xFF);
		buffer.push_back((v >> 8) & 0xFF);
		buffer.push_back((v >> 16) & 0xFF);
		buffer.push_back((v >> 24) & 0xFF);
	}
	void f64(double v) {
		uint8_t b[8];
		std::memcpy(b, &v, 8);
		buffer.insert(buffer.end(), b, b + 8);
	}
	void byteOrder() {
		u8(1);
	}
	void geomType(WKBGeometryType t) {
		u32(static_cast<uint32_t>(t));
	}
	//! Per ISO WKB, each Polygon nested under a PolyhedralSurface is its own
	//! WKB value with byte-order + type code + num_rings header.
	void polyHeader(uint32_t num_rings) {
		byteOrder();
		geomType(WKBGeometryType::PolygonZ);
		u32(num_rings);
	}
	void ring(const std::vector<Vertex3D> &pts) {
		u32(static_cast<uint32_t>(pts.size()) + 1);
		for (auto &p : pts) {
			f64(p.x);
			f64(p.y);
			f64(p.z);
		}
		f64(pts[0].x);
		f64(pts[0].y);
		f64(pts[0].z);
	}
};

//! Build a valid closed tetrahedron with consistent outward-facing winding
SolidModel MakeValidTetrahedron() {
	Vertex3D v0 = {0, 0, 0}, v1 = {1, 0, 0}, v2 = {0, 1, 0}, v3 = {0, 0, 1};

	WKBBuilder b;
	b.byteOrder();
	b.geomType(WKBGeometryType::PolyhedralSurfaceZ);
	b.u32(4);

	// Outward-facing CCW winding (when viewed from outside)
	// Face 0: v0,v1,v2 (bottom, normal points down -Z... but let's use consistent winding)
	// For a proper tetrahedron with outward normals:
	b.polyHeader(1);
	b.ring({v0, v2, v1}); // bottom face (normal -Z)
	b.polyHeader(1);
	b.ring({v0, v1, v3}); // front face
	b.polyHeader(1);
	b.ring({v1, v2, v3}); // right face
	b.polyHeader(1);
	b.ring({v2, v0, v3}); // left face

	auto surfaces = ParseWKB(b.buffer.data(), b.buffer.size());
	auto model = BuildSolidModel(surfaces);
	ValidateSolidModel(model);
	return model;
}

//! Build a unit cube with consistent outward-facing winding
SolidModel MakeValidCube() {
	Vertex3D v000 = {0, 0, 0}, v100 = {1, 0, 0}, v110 = {1, 1, 0}, v010 = {0, 1, 0};
	Vertex3D v001 = {0, 0, 1}, v101 = {1, 0, 1}, v111 = {1, 1, 1}, v011 = {0, 1, 1};

	WKBBuilder b;
	b.byteOrder();
	b.geomType(WKBGeometryType::PolyhedralSurfaceZ);
	b.u32(6);

	// Outward-facing CCW winding
	b.polyHeader(1);
	b.ring({v000, v010, v110, v100}); // bottom (normal -Z)
	b.polyHeader(1);
	b.ring({v001, v101, v111, v011}); // top (normal +Z)
	b.polyHeader(1);
	b.ring({v000, v100, v101, v001}); // front (normal -Y)
	b.polyHeader(1);
	b.ring({v010, v011, v111, v110}); // back (normal +Y)
	b.polyHeader(1);
	b.ring({v000, v001, v011, v010}); // left (normal -X)
	b.polyHeader(1);
	b.ring({v100, v110, v111, v101}); // right (normal +X)

	auto surfaces = ParseWKB(b.buffer.data(), b.buffer.size());
	auto model = BuildSolidModel(surfaces);
	ValidateSolidModel(model);
	return model;
}

//! Build an open surface (missing one face from tetrahedron)
SolidModel MakeOpenSurface() {
	Vertex3D v0 = {0, 0, 0}, v1 = {1, 0, 0}, v2 = {0, 1, 0}, v3 = {0, 0, 1};

	WKBBuilder b;
	b.byteOrder();
	b.geomType(WKBGeometryType::PolyhedralSurfaceZ);
	b.u32(3); // only 3 faces — missing one to close the tetrahedron

	b.polyHeader(1);
	b.ring({v0, v2, v1});
	b.polyHeader(1);
	b.ring({v0, v1, v3});
	b.polyHeader(1);
	b.ring({v1, v2, v3});
	// Missing: {v2, v0, v3}

	auto surfaces = ParseWKB(b.buffer.data(), b.buffer.size());
	auto model = BuildSolidModel(surfaces);
	ValidateSolidModel(model);
	return model;
}

//! Build a surface with inconsistent orientation (one face flipped)
SolidModel MakeInconsistentOrientation() {
	Vertex3D v0 = {0, 0, 0}, v1 = {1, 0, 0}, v2 = {0, 1, 0}, v3 = {0, 0, 1};

	WKBBuilder b;
	b.byteOrder();
	b.geomType(WKBGeometryType::PolyhedralSurfaceZ);
	b.u32(4);

	b.polyHeader(1);
	b.ring({v0, v2, v1}); // bottom - normal down
	b.polyHeader(1);
	b.ring({v0, v1, v3}); // front - normal out
	b.polyHeader(1);
	b.ring({v1, v2, v3}); // right - normal out
	b.polyHeader(1);
	b.ring({v0, v2, v3}); // left - FLIPPED winding (should be v2,v0,v3)

	auto surfaces = ParseWKB(b.buffer.data(), b.buffer.size());
	auto model = BuildSolidModel(surfaces);
	ValidateSolidModel(model);
	return model;
}

//! Build a surface with a degenerate face (a ring that retraces itself, so it
//! encloses exactly zero area, obliquely oriented so nothing cancels by luck),
//! optionally
//! translated by `offset` on every axis. Degeneracy is a property of the shape,
//! so the verdict must not depend on where the shape sits.
SolidModel MakeDegenerateFace(double offset = 0.0) {
	const double o = offset;
	Vertex3D v0 = {o, o, o}, v1 = {o + 1, o, o}, v2 = {o, o + 1, o}, v3 = {o, o, o + 1};

	WKBBuilder b;
	b.byteOrder();
	b.geomType(WKBGeometryType::PolyhedralSurfaceZ);
	b.u32(4);

	b.polyHeader(1);
	b.ring({v0, v2, v1});
	b.polyHeader(1);
	b.ring({v0, v1, v3});
	b.polyHeader(1);
	b.ring({v1, v2, v3});
	// Degenerate face: all points collinear
	b.polyHeader(1);
	b.ring({{o, o, o}, {o + 1, o + 0.1, o}, {o + 2, o + 0.2, o}, {o + 1, o + 0.1, o}});

	auto surfaces = ParseWKB(b.buffer.data(), b.buffer.size());
	auto model = BuildSolidModel(surfaces);
	ValidateSolidModel(model);
	return model;
}

SolidModel MakeCubeWithTopHole() {
	Vertex3D v000 = {0, 0, 0}, v100 = {1, 0, 0}, v110 = {1, 1, 0}, v010 = {0, 1, 0};
	Vertex3D v001 = {0, 0, 1}, v101 = {1, 0, 1}, v111 = {1, 1, 1}, v011 = {0, 1, 1};
	Vertex3D h00 = {0.25, 0.25, 1}, h10 = {0.75, 0.25, 1}, h11 = {0.75, 0.75, 1}, h01 = {0.25, 0.75, 1};

	WKBBuilder b;
	b.byteOrder();
	b.geomType(WKBGeometryType::PolyhedralSurfaceZ);
	b.u32(6);

	b.polyHeader(1);
	b.ring({v000, v010, v110, v100});
	b.polyHeader(2);
	b.ring({v001, v101, v111, v011});
	b.ring({h00, h01, h11, h10});
	b.polyHeader(1);
	b.ring({v000, v100, v101, v001});
	b.polyHeader(1);
	b.ring({v010, v011, v111, v110});
	b.polyHeader(1);
	b.ring({v000, v001, v011, v010});
	b.polyHeader(1);
	b.ring({v100, v110, v111, v101});

	auto surfaces = ParseWKB(b.buffer.data(), b.buffer.size());
	auto model = BuildSolidModel(surfaces);
	ValidateSolidModel(model);
	return model;
}

} // anonymous namespace

TEST_CASE("Validation: valid closed tetrahedron", "[validation]") {
	auto model = MakeValidTetrahedron();
	REQUIRE(model.validation.is_closed == true);
	REQUIRE(model.validation.is_manifold == true);
	REQUIRE(model.validation.is_oriented == true);
	REQUIRE(model.validation.is_valid == true);
	REQUIRE(model.validation.open_edge_count == 0);
	REQUIRE(model.validation.non_manifold_edge_count == 0);
	REQUIRE(model.validation.degenerate_face_count == 0);
	REQUIRE(model.validation.orientation_error_count == 0);
}

TEST_CASE("Validation: valid closed cube", "[validation]") {
	auto model = MakeValidCube();
	REQUIRE(model.validation.is_closed == true);
	REQUIRE(model.validation.is_manifold == true);
	REQUIRE(model.validation.is_oriented == true);
	REQUIRE(model.validation.is_valid == true);
	REQUIRE(model.validation.open_edge_count == 0);
}

TEST_CASE("Validation: open surface (not closed)", "[validation]") {
	auto model = MakeOpenSurface();
	REQUIRE(model.validation.is_closed == false);
	REQUIRE(model.validation.is_valid == false);
	REQUIRE(model.validation.open_edge_count > 0);
}

TEST_CASE("Validation: inconsistent orientation", "[validation]") {
	auto model = MakeInconsistentOrientation();
	REQUIRE(model.validation.is_oriented == false);
	REQUIRE(model.validation.is_valid == false);
	REQUIRE(model.validation.orientation_error_count > 0);
}

TEST_CASE("Validation: degenerate face", "[validation]") {
	auto model = MakeDegenerateFace();
	REQUIRE(model.validation.degenerate_face_count > 0);
	REQUIRE(model.validation.is_valid == false);
}

TEST_CASE("Validation: the degeneracy verdict does not depend on position", "[validation]") {
	// A Newell area computed on absolute coordinates carries n*eps*|position|*
	// |extent| of noise, 1e-11 to 1e-10 at RD New (EPSG:28992) easting and
	// northing, so it is computed about the ring's first vertex
	// (geometry_math.cpp). degenerate_face_count gates ComputeVolume and
	// ComputeSurfaceArea, so a position-dependent verdict would make a hard error
	// position-dependent.
	for (double offset : {0.0, 1.0e2, 8.5e4, 4.47e5, 1.0e6, 1.0e8}) {
		auto model = MakeDegenerateFace(offset);
		INFO("offset = " << offset);
		REQUIRE(model.validation.degenerate_face_count > 0);
		REQUIRE(model.validation.is_valid == false);
	}
}

TEST_CASE("Validation: hole boundaries count as open edges", "[validation]") {
	auto model = MakeCubeWithTopHole();
	REQUIRE(model.validation.is_closed == false);
	REQUIRE(model.validation.is_valid == false);
	REQUIRE(model.validation.open_edge_count >= 4);
}

namespace {

using Ring = std::vector<Vertex3D>;
using Face = std::vector<Ring>;

//! Builds and validates a model from faces, optionally split into shells by
//! per-shell face counts (one solid).
SolidModel BuildFaces(const std::vector<Face> &faces, std::vector<uint32_t> shell_face_counts = {}) {
	WKBBuilder b;
	b.byteOrder();
	b.geomType(WKBGeometryType::PolyhedralSurfaceZ);
	b.u32(static_cast<uint32_t>(faces.size()));
	for (auto &face : faces) {
		b.polyHeader(static_cast<uint32_t>(face.size()));
		for (auto &ring : face) {
			b.ring(ring);
		}
	}
	GeometryMetadata md;
	if (!shell_face_counts.empty()) {
		md.shells = ShellCounts {shell_face_counts};
	}
	auto model = BuildSolidModel(ParseWKB(b.buffer.data(), b.buffer.size()), md);
	ValidateSolidModel(model);
	return model;
}

//! The six faces of an axis-aligned box, wound outward (or inward).
std::vector<Face> BoxFaces(Vertex3D lo, Vertex3D hi, bool inward = false) {
	Vertex3D v000 = {lo.x, lo.y, lo.z}, v100 = {hi.x, lo.y, lo.z}, v110 = {hi.x, hi.y, lo.z}, v010 = {lo.x, hi.y, lo.z};
	Vertex3D v001 = {lo.x, lo.y, hi.z}, v101 = {hi.x, lo.y, hi.z}, v111 = {hi.x, hi.y, hi.z}, v011 = {lo.x, hi.y, hi.z};
	std::vector<Face> faces = {{{v000, v010, v110, v100}}, {{v001, v101, v111, v011}}, {{v000, v100, v101, v001}},
	                           {{v010, v011, v111, v110}}, {{v000, v001, v011, v010}}, {{v100, v110, v111, v101}}};
	if (inward) {
		for (auto &f : faces) {
			std::reverse(f[0].begin(), f[0].end());
		}
	}
	return faces;
}

std::vector<Face> Concat(std::vector<Face> a, const std::vector<Face> &b) {
	a.insert(a.end(), b.begin(), b.end());
	return a;
}

} // anonymous namespace

TEST_CASE("Validation: two boxes sharing one vertex are two parts of one shell", "[validation]") {
	// val3dity 305 (MULTIPLE_CONNECTED_COMPONENTS). The shared vertex joins two
	// separate parts; it is not a pinched vertex of one part.
	auto model = BuildFaces(Concat(BoxFaces({0, 0, 0}, {1, 1, 1}), BoxFaces({1, 1, 1}, {2, 2, 2})));
	REQUIRE(model.validation.disconnected_shell_count == 1);
	REQUIRE(model.validation.non_manifold_vertex_count == 0);
	REQUIRE(model.validation.self_intersection_count == 0);
	REQUIRE(model.validation.is_valid == false);
}

TEST_CASE("Validation: two disjoint boxes in one shell are disconnected", "[validation]") {
	auto model = BuildFaces(Concat(BoxFaces({0, 0, 0}, {1, 1, 1}), BoxFaces({3, 0, 0}, {4, 1, 1})));
	REQUIRE(model.validation.disconnected_shell_count == 1);
	REQUIRE(model.validation.is_closed == true);
	REQUIRE(model.validation.is_valid == false);
}

TEST_CASE("Validation: two overlapping boxes in one shell intersect", "[validation]") {
	// The review's case: ST_3DVolume gave 16 for a union of 12.625.
	auto model = BuildFaces(Concat(BoxFaces({0, 0, 0}, {2, 2, 2}), BoxFaces({0.5, 0.5, 0.5}, {2.5, 2.5, 2.5})));
	REQUIRE(model.validation.self_intersection_count > 0);
	REQUIRE(model.validation.is_valid == false);
	// The divergence sum counts the overlap twice; volume refuses instead.
	REQUIRE_THROWS_WITH(ComputeVolume(model), Catch::Contains("self-intersecting"));
}

TEST_CASE("Validation: an interior shell crossing the exterior intersects it", "[validation]") {
	auto faces = Concat(BoxFaces({0, 0, 0}, {10, 10, 10}), BoxFaces({4, 4, 4}, {6, 6, 12}, true));
	auto model = BuildFaces(faces, {6, 6});
	REQUIRE(model.ShellCount() == 2);
	REQUIRE(model.validation.disconnected_shell_count == 0);
	// The cavity's four side faces each cross the exterior's top face.
	REQUIRE(model.validation.self_intersection_count == 4);
	REQUIRE(model.validation.is_valid == false);
}

TEST_CASE("Validation: a cavity strictly inside the exterior is valid", "[validation]") {
	auto faces = Concat(BoxFaces({0, 0, 0}, {10, 10, 10}), BoxFaces({4, 4, 4}, {6, 6, 6}, true));
	auto model = BuildFaces(faces, {6, 6});
	REQUIRE(model.validation.self_intersection_count == 0);
	REQUIRE(model.validation.disconnected_shell_count == 0);
	REQUIRE(model.validation.is_valid == true);
}

TEST_CASE("Validation: an interior shell outside the exterior is not a cavity", "[validation]") {
	// val3dity 403 (INNER_SHELL_OUTSIDE). Correctly wound and smaller than the
	// exterior, it crosses nothing, but its volume would still be subtracted.
	auto faces = Concat(BoxFaces({0, 0, 0}, {10, 10, 10}), BoxFaces({20, 20, 20}, {22, 22, 22}, true));
	auto model = BuildFaces(faces, {6, 6});
	REQUIRE(model.validation.self_intersection_count == 0);
	REQUIRE(model.validation.orientation_error_count == 1);
	REQUIRE(model.validation.is_valid == false);
	REQUIRE_THROWS(ComputeVolume(model));
}

TEST_CASE("Validation: a cavity inside another cavity is not a cavity", "[validation]") {
	// val3dity 401 (INTERSECTION_SHELLS): the inner one's volume would be
	// subtracted twice.
	auto faces = Concat(BoxFaces({0, 0, 0}, {10, 10, 10}),
	                    Concat(BoxFaces({2, 2, 2}, {8, 8, 8}, true), BoxFaces({3, 3, 3}, {4, 4, 4}, true)));
	auto model = BuildFaces(faces, {6, 6, 6});
	REQUIRE(model.validation.self_intersection_count == 0);
	REQUIRE(model.validation.orientation_error_count == 1);
	REQUIRE(model.validation.is_valid == false);
}

TEST_CASE("Validation: two separate cavities are valid", "[validation]") {
	auto faces = Concat(BoxFaces({0, 0, 0}, {10, 10, 10}),
	                    Concat(BoxFaces({2, 2, 2}, {4, 4, 4}, true), BoxFaces({6, 6, 6}, {8, 8, 8}, true)));
	auto model = BuildFaces(faces, {6, 6, 6});
	REQUIRE(model.validation.orientation_error_count == 0);
	REQUIRE(model.validation.is_valid == true);
	REQUIRE(ComputeVolume(model) == Approx(1000.0 - 8.0 - 8.0));
}

TEST_CASE("Validation: a concave prism and a split face are not self-intersections", "[validation]") {
	// An L-shaped prism whose top is two coplanar faces sharing an edge.
	std::vector<Vertex3D> l = {{0, 0, 0}, {2, 0, 0}, {2, 1, 0}, {1, 1, 0}, {1, 2, 0}, {0, 2, 0}};
	auto up = [](Vertex3D v) {
		return Vertex3D {v.x, v.y, 3};
	};
	std::vector<Face> faces;
	faces.push_back({{l[0], l[5], l[4], l[3], l[2], l[1]}}); // bottom, outward -Z
	faces.push_back({{up(l[0]), up(l[1]), up(l[2]), up(l[3])}});
	faces.push_back({{up(l[0]), up(l[3]), up(l[4]), up(l[5])}});
	for (size_t i = 0; i < l.size(); i++) {
		auto a = l[i], b2 = l[(i + 1) % l.size()];
		faces.push_back({{a, b2, up(b2), up(a)}});
	}
	auto model = BuildFaces(faces);
	REQUIRE(model.validation.is_closed == true);
	REQUIRE(model.validation.is_oriented == true);
	REQUIRE(model.validation.self_intersection_count == 0);
	REQUIRE(model.validation.non_manifold_vertex_count == 0);
	REQUIRE(model.validation.disconnected_shell_count == 0);
	REQUIRE(model.validation.is_valid == true);
}

TEST_CASE("Validation: faces meeting at a vertex in two fans make it non-manifold", "[validation]") {
	// Two triangles touch only at p, and a quad joins them along edges ab and
	// cd, so the surface is one part with a pinched vertex (val3dity 303).
	Vertex3D a = {0, 0, 0}, b = {1, 0, 0}, c = {1, 2, 0}, d = {0, 2, 0}, p = {0.5, 1, 1};
	auto model = BuildFaces({{{a, b, p}}, {{p, c, d}}, {{b, a, d, c}}});
	REQUIRE(model.validation.disconnected_shell_count == 0);
	REQUIRE(model.validation.non_manifold_vertex_count == 1);
	REQUIRE(model.validation.is_manifold == false);
	REQUIRE(model.validation.is_valid == false);
}

TEST_CASE("Validation: penetration within the faces' non-planarity is not an intersection", "[validation]") {
	// The exterior's top face is non-planar: one corner is raised by e, so it
	// deviates from its plane by about e/4. A cavity poking through that face by
	// less than the deviation is within tolerance; by much more, it is not.
	const double e = 0.01;
	auto exterior = BoxFaces({0, 0, 0}, {10, 10, 10});
	exterior[1][0][2].z += e; // the top face's (10, 10, 10) corner
	exterior[3][0][2].z += e; // the same vertex in the back face
	exterior[5][0][2].z += e; // and in the right face
	SECTION("within") {
		auto model = BuildFaces(Concat(exterior, BoxFaces({4, 4, 4}, {6, 6, 10 + 0.02 * e}, true)), {6, 6});
		REQUIRE(model.validation.self_intersection_count == 0);
	}
	SECTION("beyond") {
		auto model = BuildFaces(Concat(exterior, BoxFaces({4, 4, 4}, {6, 6, 10 + 3 * e}, true)), {6, 6});
		REQUIRE(model.validation.self_intersection_count == 4);
	}
}

TEST_CASE("Validation: a thin face crossing warped walls still intersects them", "[validation]") {
	// A 3 m x 5 cm strip pierces the cube's x = 0 and x = 1 walls by a metre on
	// each side. Pushing one corner of each wall 4 cm out of its plane makes the
	// walls non-planar by about a centimetre, which moves where they are by that
	// much, not by a metre: the strip still crosses both.
	const Face strip = {{{-1, 0.40, 0.5}, {2, 0.40, 0.5}, {2, 0.45, 0.5}, {-1, 0.45, 0.5}}};
	for (double warp : {0.0, 0.04}) {
		auto cube = BoxFaces({0, 0, 0}, {1, 1, 1});
		cube[4][0][2].x -= warp; // (0, 1, 1) in the x = 0 wall,
		cube[1][0][3].x -= warp; // the top
		cube[3][0][1].x -= warp; // and the back
		cube[5][0][2].x += warp; // (1, 1, 1) in the x = 1 wall,
		cube[1][0][2].x += warp; // the top
		cube[3][0][2].x += warp; // and the back
		auto model = BuildFaces(Concat(cube, {strip}));
		INFO("warp = " << warp);
		REQUIRE(model.validation.self_intersection_count == 2);
	}
}

TEST_CASE("Validation: a thin slab with a raised top vertex does not intersect itself", "[validation]") {
	// A 10 x 10 x 0.1 m slab whose top face carries a vertex raised 0.1 m in the
	// middle of an edge: the top is non-planar by more than the slab is thick,
	// but lies strictly above the bottom everywhere. Faces within tolerance of
	// each other's planes that do not cross are not an intersection.
	for (double t : {0.1, 0.05, 0.02}) {
		Vertex3D v000 = {0, 0, 0}, v100 = {10, 0, 0}, v110 = {10, 10, 0}, v010 = {0, 10, 0};
		Vertex3D v001 = {0, 0, t}, v101 = {10, 0, t}, v111 = {10, 10, t}, v011 = {0, 10, t};
		Vertex3D raised = {5, 0, 2 * t};
		std::vector<Face> slab = {{{v000, v010, v110, v100}},         {{v001, raised, v101, v111, v011}},
		                          {{v000, v100, v101, raised, v001}}, {{v010, v011, v111, v110}},
		                          {{v000, v001, v011, v010}},         {{v100, v110, v111, v101}}};
		auto model = BuildFaces(slab);
		INFO("thickness = " << t);
		REQUIRE(model.validation.is_closed == true);
		REQUIRE(model.validation.self_intersection_count == 0);
		REQUIRE(model.validation.is_valid == true);
	}
}

TEST_CASE("Validation: the self-intersection verdict survives a rotation", "[validation]") {
	// 3DBAG NL.IMBAG.Pand.0503100000005143-0 at LoD 2.2 (Delft tile), valid by
	// val3dity. One wall carries a middle vertex on its vertical edge; rotated
	// about X, that wall's triangulation includes the three collinear vertices as
	// a zero-width triangle that meets the neighbouring wall at one point. That
	// is contact, not an overlap, whatever the rotation.
	const std::vector<Face> building = {
	    {{{84787.603, 446353.032, 0.518},
	      {84785.526, 446352.152, 0.518},
	      {84784.054, 446351.529, 0.518},
	      {84783.979, 446351.497, 0.518},
	      {84785.404, 446348.135, 0.518},
	      {84776.569, 446344.417, 0.518},
	      {84775.164, 446343.826, 0.518},
	      {84774.627, 446345.116, 0.518},
	      {84776.006, 446345.700, 0.518},
	      {84773.826, 446350.846, 0.518},
	      {84773.984, 446350.912, 0.518},
	      {84782.754, 446354.604, 0.518},
	      {84786.284, 446356.090, 0.518}}},
	    {{{84776.569, 446344.417, 6.360},
	      {84776.569, 446344.417, 3.364},
	      {84776.569, 446344.417, 0.518},
	      {84785.404, 446348.135, 0.518},
	      {84785.404, 446348.135, 6.300}}},
	    {{{84775.164, 446343.826, 3.232},
	      {84775.164, 446343.826, 0.518},
	      {84776.569, 446344.417, 0.518},
	      {84776.569, 446344.417, 3.364}}},
	    {{{84774.627, 446345.116, 3.283},
	      {84774.627, 446345.116, 0.518},
	      {84775.164, 446343.826, 0.518},
	      {84775.164, 446343.826, 3.232}}},
	    {{{84776.006, 446345.700, 3.413},
	      {84776.006, 446345.700, 0.518},
	      {84774.627, 446345.116, 0.518},
	      {84774.627, 446345.116, 3.283}}},
	    {{{84785.404, 446348.135, 6.300},
	      {84785.404, 446348.135, 0.518},
	      {84783.979, 446351.497, 0.518},
	      {84783.979, 446351.497, 6.325}}},
	    {{{84783.979, 446351.497, 6.325},
	      {84783.979, 446351.497, 0.518},
	      {84784.054, 446351.529, 0.518},
	      {84784.054, 446351.529, 4.328},
	      {84784.054, 446351.529, 6.324}}},
	    {{{84783.717, 446352.145, 6.329},
	      {84783.717, 446352.145, 3.555},
	      {84782.754, 446354.604, 3.578},
	      {84782.754, 446354.604, 6.346}}},
	    {{{84783.951, 446352.146, 6.327},
	      {84783.951, 446352.146, 4.363},
	      {84783.951, 446352.146, 3.553},
	      {84783.717, 446352.145, 3.555},
	      {84783.717, 446352.145, 6.329}}},
	    {{{84787.603, 446353.032, 3.530},
	      {84787.603, 446353.032, 0.518},
	      {84786.284, 446356.090, 0.518},
	      {84786.284, 446356.090, 3.559}}},
	    {{{84773.826, 446350.846, 3.613},
	      {84773.826, 446350.846, 0.518},
	      {84776.006, 446345.700, 0.518},
	      {84776.006, 446345.700, 3.413}}},
	    {{{84773.984, 446350.912, 3.628},
	      {84773.984, 446350.912, 0.518},
	      {84773.826, 446350.846, 0.518},
	      {84773.826, 446350.846, 3.613}}},
	    {{{84786.284, 446356.090, 3.559},
	      {84786.284, 446356.090, 0.518},
	      {84782.754, 446354.604, 0.518},
	      {84782.754, 446354.604, 3.578}}},
	    {{{84782.754, 446354.604, 6.346},
	      {84782.754, 446354.604, 3.578},
	      {84782.754, 446354.604, 0.518},
	      {84773.984, 446350.912, 0.518},
	      {84773.984, 446350.912, 3.628},
	      {84773.984, 446350.912, 6.405}}},
	    {{{84784.054, 446351.529, 4.328},
	      {84784.054, 446351.529, 0.518},
	      {84785.526, 446352.152, 0.518},
	      {84785.526, 446352.152, 3.541},
	      {84785.526, 446352.152, 4.466}}},
	    {{{84785.526, 446352.152, 4.466},
	      {84785.526, 446352.152, 3.541},
	      {84783.951, 446352.146, 3.553},
	      {84783.951, 446352.146, 4.363}}},
	    {{{84784.054, 446351.529, 6.324},
	      {84784.054, 446351.529, 4.328},
	      {84783.951, 446352.146, 4.363},
	      {84783.951, 446352.146, 6.327}}},
	    {{{84785.526, 446352.152, 3.541},
	      {84785.526, 446352.152, 0.518},
	      {84787.603, 446353.032, 0.518},
	      {84787.603, 446353.032, 3.530}}},
	    {{{84773.984, 446350.912, 6.405},
	      {84773.984, 446350.912, 3.628},
	      {84776.569, 446344.417, 3.364},
	      {84776.569, 446344.417, 6.360}}},
	    {{{84776.569, 446344.417, 3.364},
	      {84773.984, 446350.912, 3.628},
	      {84773.826, 446350.846, 3.613},
	      {84776.006, 446345.700, 3.413},
	      {84774.627, 446345.116, 3.283},
	      {84775.164, 446343.826, 3.232}}},
	    {{{84783.951, 446352.146, 4.363}, {84784.054, 446351.529, 4.328}, {84785.526, 446352.152, 4.466}}},
	    {{{84785.526, 446352.152, 3.541},
	      {84787.603, 446353.032, 3.530},
	      {84786.284, 446356.090, 3.559},
	      {84782.754, 446354.604, 3.578},
	      {84783.717, 446352.145, 3.555},
	      {84783.951, 446352.146, 3.553}}},
	    {{{84773.984, 446350.912, 6.405},
	      {84776.569, 446344.417, 6.360},
	      {84785.404, 446348.135, 6.300},
	      {84783.979, 446351.497, 6.325},
	      {84784.054, 446351.529, 6.324},
	      {84783.951, 446352.146, 6.327},
	      {84783.717, 446352.145, 6.329},
	      {84782.754, 446354.604, 6.346}}}};
	for (double angle : {0.0, 0.3, 0.7, 1.1, 2.0}) {
		auto faces = building;
		for (auto &face : faces) {
			for (auto &ring : face) {
				for (auto &v : ring) {
					double y = v.y, z = v.z;
					v.y = std::cos(angle) * y - std::sin(angle) * z;
					v.z = std::sin(angle) * y + std::cos(angle) * z;
				}
			}
		}
		auto model = BuildFaces(faces);
		INFO("angle = " << angle);
		REQUIRE(model.validation.is_closed == true);
		REQUIRE(model.validation.self_intersection_count == 0);
		REQUIRE(model.validation.is_valid == true);
	}
}

TEST_CASE("Validation: the verdict does not depend on the unit of length", "[validation]") {
	// 3DBAG NL.IMBAG.Pand.0503100000030798-0 (LoD2.2, Delft), which val3dity
	// accepts. One of its faces is a triangle with 1 mm legs (area 5e-7 m2);
	// the same building in kilometres, or in thousands of kilometres, is still
	// a valid solid with no degenerate face.
	const std::vector<Face> building = {
	    {{{84918.315, 446687.669, 0.054},
	      {84916.709, 446686.798, 0.054},
	      {84916.404, 446686.633, 0.054},
	      {84915.446, 446688.446, 0.054},
	      {84914.773, 446688.092, 0.054},
	      {84913.863, 446688.405, 0.054},
	      {84912.224, 446691.557, 0.054},
	      {84913.215, 446692.102, 0.054},
	      {84915.461, 446693.338, 0.054},
	      {84918.277, 446694.887, 0.054},
	      {84923.255, 446697.626, 0.054},
	      {84924.798, 446694.668, 0.054},
	      {84926.215, 446691.954, 0.054},
	      {84921.230, 446689.251, 0.054}}},
	    {{{84913.863, 446688.405, 6.975},
	      {84913.863, 446688.405, 0.054},
	      {84914.773, 446688.092, 0.054},
	      {84914.773, 446688.092, 7.394}}},
	    {{{84914.773, 446688.092, 7.394},
	      {84914.773, 446688.092, 0.054},
	      {84915.446, 446688.446, 0.054},
	      {84915.446, 446688.446, 7.886}}},
	    {{{84912.224, 446691.557, 7.021},
	      {84912.224, 446691.557, 0.054},
	      {84913.863, 446688.405, 0.054},
	      {84913.863, 446688.405, 6.975}}},
	    {{{84915.446, 446688.446, 7.886},
	      {84915.446, 446688.446, 0.054},
	      {84916.404, 446686.633, 0.054},
	      {84916.404, 446686.633, 7.868}}},
	    {{{84919.986, 446692.032, 10.354},
	      {84919.986, 446692.032, 9.230},
	      {84920.605, 446690.997, 9.195},
	      {84920.605, 446690.997, 9.924}}},
	    {{{84921.230, 446689.251, 9.271},
	      {84921.230, 446689.251, 0.054},
	      {84926.215, 446691.954, 0.054},
	      {84926.215, 446691.954, 9.241}}},
	    {{{84918.277, 446694.887, 9.327},
	      {84918.277, 446694.887, 9.199},
	      {84918.277, 446694.887, 0.054},
	      {84915.461, 446693.338, 0.054},
	      {84915.461, 446693.338, 10.487}}},
	    {{{84915.461, 446693.338, 10.487},
	      {84915.461, 446693.338, 0.054},
	      {84913.215, 446692.102, 0.054},
	      {84913.215, 446692.102, 7.752},
	      {84913.215, 446692.102, 9.093}}},
	    {{{84924.798, 446694.668, 10.333},
	      {84924.798, 446694.668, 0.054},
	      {84923.255, 446697.626, 0.054},
	      {84923.255, 446697.626, 9.173}}},
	    {{{84913.215, 446692.102, 7.752},
	      {84913.215, 446692.102, 0.054},
	      {84912.224, 446691.557, 0.054},
	      {84912.224, 446691.557, 7.021}}},
	    {{{84916.404, 446686.633, 7.868},
	      {84916.404, 446686.633, 0.054},
	      {84916.709, 446686.798, 0.054},
	      {84916.709, 446686.798, 8.092}}},
	    {{{84923.255, 446697.626, 9.173},
	      {84923.255, 446697.626, 0.054},
	      {84918.277, 446694.887, 0.054},
	      {84918.277, 446694.887, 9.199}}},
	    {{{84919.986, 446692.032, 9.230}, {84919.986, 446692.032, 10.354}, {84918.452, 446694.596, 9.317}}},
	    {{{84918.277, 446694.887, 9.199}, {84918.277, 446694.887, 9.327}, {84918.452, 446694.596, 9.317}}},
	    {{{84926.215, 446691.954, 9.241},
	      {84926.215, 446691.954, 0.054},
	      {84924.798, 446694.668, 0.054},
	      {84924.798, 446694.668, 10.333}}},
	    {{{84918.315, 446687.669, 10.470},
	      {84918.315, 446687.669, 0.054},
	      {84921.230, 446689.251, 0.054},
	      {84921.230, 446689.251, 9.271},
	      {84921.230, 446689.251, 9.272}}},
	    {{{84921.230, 446689.251, 9.272}, {84921.230, 446689.251, 9.271}, {84921.230, 446689.252, 9.272}}},
	    {{{84920.605, 446690.997, 9.924}, {84920.605, 446690.997, 9.195}, {84921.230, 446689.252, 9.272}}},
	    {{{84916.709, 446686.798, 9.476},
	      {84916.709, 446686.798, 8.092},
	      {84916.709, 446686.798, 0.054},
	      {84918.315, 446687.669, 0.054},
	      {84918.315, 446687.669, 10.470}}},
	    {{{84913.215, 446692.102, 9.093},
	      {84913.215, 446692.102, 7.752},
	      {84916.709, 446686.798, 8.092},
	      {84916.709, 446686.798, 9.476}}},
	    {{{84918.277, 446694.887, 9.199},
	      {84918.452, 446694.596, 9.317},
	      {84919.986, 446692.032, 10.354},
	      {84924.798, 446694.668, 10.333},
	      {84923.255, 446697.626, 9.173}}},
	    {{{84920.605, 446690.997, 9.924},
	      {84921.230, 446689.252, 9.272},
	      {84921.230, 446689.251, 9.271},
	      {84926.215, 446691.954, 9.241},
	      {84924.798, 446694.668, 10.333},
	      {84919.986, 446692.032, 10.354}}},
	    {{{84916.709, 446686.798, 8.092},
	      {84913.215, 446692.102, 7.752},
	      {84912.224, 446691.557, 7.021},
	      {84913.863, 446688.405, 6.975},
	      {84914.773, 446688.092, 7.394},
	      {84915.446, 446688.446, 7.886},
	      {84916.404, 446686.633, 7.868}}},
	    {{{84915.461, 446693.338, 10.487},
	      {84918.315, 446687.669, 10.470},
	      {84921.230, 446689.251, 9.272},
	      {84921.230, 446689.252, 9.272},
	      {84920.605, 446690.997, 9.195},
	      {84919.986, 446692.032, 9.230},
	      {84918.452, 446694.596, 9.317},
	      {84918.277, 446694.887, 9.327}}},
	    {{{84918.315, 446687.669, 10.470},
	      {84915.461, 446693.338, 10.487},
	      {84913.215, 446692.102, 9.093},
	      {84916.709, 446686.798, 9.476}}}};
	for (double scale : {1.0, 1.0e-3, 1.0e-6}) {
		auto faces = building;
		for (auto &face : faces) {
			for (auto &ring : face) {
				for (auto &v : ring) {
					v = {v.x * scale, v.y * scale, v.z * scale};
				}
			}
		}
		auto model = BuildFaces(faces);
		INFO("scale = " << scale);
		REQUIRE(model.validation.degenerate_face_count == 0);
		REQUIRE(model.validation.is_valid == true);
	}
}
