#include "catch.hpp"
#include "kernel/triangulation.hpp"
#include "kernel/measurements.hpp"
#include "kernel/model_builder.hpp"
#include "kernel/wkb_parser.hpp"
#include <cstring>
#include <cmath>

using namespace duckdb_3d;

namespace {

class WKBBuilder {
public:
	std::vector<uint8_t> buffer;
	void u8(uint8_t v) { buffer.push_back(v); }
	void u32(uint32_t v) {
		buffer.push_back(v & 0xFF); buffer.push_back((v >> 8) & 0xFF);
		buffer.push_back((v >> 16) & 0xFF); buffer.push_back((v >> 24) & 0xFF);
	}
	void f64(double v) {
		uint8_t b[8]; std::memcpy(b, &v, 8);
		buffer.insert(buffer.end(), b, b + 8);
	}
	void byteOrder() { u8(1); }
	void geomType(WKBGeometryType t) { u32(static_cast<uint32_t>(t)); }
	void ring(const std::vector<Vertex3D> &pts) {
		u32(static_cast<uint32_t>(pts.size()) + 1);
		for (auto &p : pts) { f64(p.x); f64(p.y); f64(p.z); }
		f64(pts[0].x); f64(pts[0].y); f64(pts[0].z);
	}
};

//! Build a valid unit cube with consistent outward-facing winding
SolidModel MakeValidCube() {
	Vertex3D v000={0,0,0}, v100={1,0,0}, v110={1,1,0}, v010={0,1,0};
	Vertex3D v001={0,0,1}, v101={1,0,1}, v111={1,1,1}, v011={0,1,1};

	WKBBuilder b;
	b.byteOrder();
	b.geomType(WKBGeometryType::PolyhedralSurfaceZ);
	b.u32(6);
	b.u32(1); b.ring({v000, v010, v110, v100}); // bottom
	b.u32(1); b.ring({v001, v101, v111, v011}); // top
	b.u32(1); b.ring({v000, v100, v101, v001}); // front
	b.u32(1); b.ring({v010, v011, v111, v110}); // back
	b.u32(1); b.ring({v000, v001, v011, v010}); // left
	b.u32(1); b.ring({v100, v110, v111, v101}); // right

	auto surfaces = ParseWKB(b.buffer.data(), b.buffer.size());
	auto model = BuildSolidModel(surfaces);
	TriangulateSolidModel(model);
	return model;
}

//! Build a valid tetrahedron
SolidModel MakeValidTetrahedron() {
	Vertex3D v0 = {0,0,0}, v1 = {1,0,0}, v2 = {0,1,0}, v3 = {0,0,1};

	WKBBuilder b;
	b.byteOrder();
	b.geomType(WKBGeometryType::PolyhedralSurfaceZ);
	b.u32(4);
	b.u32(1); b.ring({v0, v2, v1});
	b.u32(1); b.ring({v0, v1, v3});
	b.u32(1); b.ring({v1, v2, v3});
	b.u32(1); b.ring({v2, v0, v3});

	auto surfaces = ParseWKB(b.buffer.data(), b.buffer.size());
	auto model = BuildSolidModel(surfaces);
	TriangulateSolidModel(model);
	return model;
}

} // anonymous namespace

TEST_CASE("Triangulation: cube has 12 triangles (2 per quad face)", "[triangulation]") {
	auto model = MakeValidCube();
	REQUIRE(model.TriangleCount() == 12);
	REQUIRE(model.triangle_vertex_indices.size() == 36);
}

TEST_CASE("Triangulation: tetrahedron has 4 triangles", "[triangulation]") {
	auto model = MakeValidTetrahedron();
	REQUIRE(model.TriangleCount() == 4);
}

TEST_CASE("Surface area: unit cube = 6.0", "[measurements]") {
	auto model = MakeValidCube();
	double area = ComputeSurfaceArea(model);
	REQUIRE(area == Approx(6.0).epsilon(1e-10));
}

TEST_CASE("Surface area: tetrahedron", "[measurements]") {
	// Tetrahedron with vertices (0,0,0), (1,0,0), (0,1,0), (0,0,1)
	// Face areas:
	// Bottom (0,0,0)-(1,0,0)-(0,1,0): right triangle, area = 0.5
	// Front (0,0,0)-(1,0,0)-(0,0,1): right triangle, area = 0.5
	// Left (0,0,0)-(0,1,0)-(0,0,1): right triangle, area = 0.5
	// Hypotenuse face (1,0,0)-(0,1,0)-(0,0,1): area = sqrt(3)/2 ≈ 0.866
	auto model = MakeValidTetrahedron();
	double area = ComputeSurfaceArea(model);
	double expected = 3 * 0.5 + std::sqrt(3.0) / 2.0;
	REQUIRE(area == Approx(expected).epsilon(1e-10));
}

TEST_CASE("Volume: unit cube = 1.0", "[measurements]") {
	auto model = MakeValidCube();
	double vol = ComputeVolume(model);
	REQUIRE(vol == Approx(1.0).epsilon(1e-10));
}

TEST_CASE("Volume: tetrahedron = 1/6", "[measurements]") {
	// V = |det([v1-v0, v2-v0, v3-v0])| / 6 = |det([[1,0,0],[0,1,0],[0,0,1]])| / 6 = 1/6
	auto model = MakeValidTetrahedron();
	double vol = ComputeVolume(model);
	REQUIRE(vol == Approx(1.0 / 6.0).epsilon(1e-10));
}

TEST_CASE("Volume: multi-solid sums volumes", "[measurements]") {
	// Two identical cubes
	auto cube_wkb_fn = []() {
		Vertex3D v000={0,0,0}, v100={1,0,0}, v110={1,1,0}, v010={0,1,0};
		Vertex3D v001={0,0,1}, v101={1,0,1}, v111={1,1,1}, v011={0,1,1};
		WKBBuilder b;
		b.byteOrder();
		b.geomType(WKBGeometryType::PolyhedralSurfaceZ);
		b.u32(6);
		b.u32(1); b.ring({v000, v010, v110, v100});
		b.u32(1); b.ring({v001, v101, v111, v011});
		b.u32(1); b.ring({v000, v100, v101, v001});
		b.u32(1); b.ring({v010, v011, v111, v110});
		b.u32(1); b.ring({v000, v001, v011, v010});
		b.u32(1); b.ring({v100, v110, v111, v101});
		return b.buffer;
	};

	auto cube1 = cube_wkb_fn();
	WKBBuilder gc;
	gc.byteOrder();
	gc.geomType(WKBGeometryType::GeometryCollectionZ);
	gc.u32(2);
	gc.buffer.insert(gc.buffer.end(), cube1.begin(), cube1.end());
	gc.buffer.insert(gc.buffer.end(), cube1.begin(), cube1.end());

	auto surfaces = ParseWKB(gc.buffer.data(), gc.buffer.size());
	auto model = BuildSolidModel(surfaces);
	TriangulateSolidModel(model);

	double vol = ComputeVolume(model);
	REQUIRE(vol == Approx(2.0).epsilon(1e-10));
}
