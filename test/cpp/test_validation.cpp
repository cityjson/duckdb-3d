#include "catch.hpp"
#include "kernel/validation.hpp"
#include "kernel/model_builder.hpp"
#include "kernel/wkb_parser.hpp"
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

//! Build a surface with a degenerate face (all vertices collinear)
SolidModel MakeDegenerateFace() {
	Vertex3D v0 = {0, 0, 0}, v1 = {1, 0, 0}, v2 = {0, 1, 0}, v3 = {0, 0, 1};

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
	b.ring({{0, 0, 0}, {0.5, 0, 0}, {1, 0, 0}});

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

TEST_CASE("Validation: hole boundaries count as open edges", "[validation]") {
	auto model = MakeCubeWithTopHole();
	REQUIRE(model.validation.is_closed == false);
	REQUIRE(model.validation.is_valid == false);
	REQUIRE(model.validation.open_edge_count >= 4);
}
