#include "catch.hpp"
#include "kernel/wkb_export.hpp"
#include "kernel/wkb_parser.hpp"
#include "kernel/model_builder.hpp"
#include "kernel/measurements.hpp"
#include "kernel/triangulation.hpp"
#include <cstring>
#include <cmath>

// Direct unit coverage for kernel/wkb_export (ExportWKB). The SQL layer exercises
// ST_3DAsWKB, but the kernel round-trip (model -> WKB -> model) had no dedicated
// C++ test. A lossless round-trip must preserve topology counts, bbox, and volume.

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

std::vector<uint8_t> BuildUnitCubeWKB() {
	Vertex3D v000 = {0, 0, 0}, v100 = {1, 0, 0}, v110 = {1, 1, 0}, v010 = {0, 1, 0};
	Vertex3D v001 = {0, 0, 1}, v101 = {1, 0, 1}, v111 = {1, 1, 1}, v011 = {0, 1, 1};
	WKBBuilder b;
	b.byteOrder();
	b.geomType(WKBGeometryType::PolyhedralSurfaceZ);
	b.u32(6);
	b.polyHeader(1);
	b.ring({v000, v010, v110, v100});
	b.polyHeader(1);
	b.ring({v001, v101, v111, v011});
	b.polyHeader(1);
	b.ring({v000, v100, v101, v001});
	b.polyHeader(1);
	b.ring({v010, v011, v111, v110});
	b.polyHeader(1);
	b.ring({v000, v001, v011, v010});
	b.polyHeader(1);
	b.ring({v100, v110, v111, v101});
	return b.buffer;
}

SolidModel BuildModel(const std::vector<uint8_t> &wkb) {
	auto surfaces = ParseWKB(wkb.data(), wkb.size());
	auto model = BuildSolidModel(surfaces);
	TriangulateSolidModel(model);
	return model;
}

} // namespace

TEST_CASE("ExportWKB: unit cube round-trips losslessly (counts, bbox, volume)", "[wkb_export]") {
	auto original = BuildModel(BuildUnitCubeWKB());

	auto exported = ExportWKB(original);
	REQUIRE(exported.size() > 0);

	auto restored = BuildModel(exported);
	REQUIRE(restored.SolidCount() == original.SolidCount());
	REQUIRE(restored.ShellCount() == original.ShellCount());
	REQUIRE(restored.FaceCount() == original.FaceCount());
	REQUIRE(restored.vertices.size() == original.vertices.size());

	REQUIRE(restored.bbox.min_x == Approx(original.bbox.min_x));
	REQUIRE(restored.bbox.max_z == Approx(original.bbox.max_z));
	REQUIRE(ComputeVolume(restored) == Approx(ComputeVolume(original)).epsilon(1e-12));
}

TEST_CASE("ExportWKB: single solid exports as PolyhedralSurface Z", "[wkb_export]") {
	auto model = BuildModel(BuildUnitCubeWKB());
	auto exported = ExportWKB(model);

	// Byte 0 = little-endian order; bytes 1..4 = geometry type code.
	REQUIRE(exported[0] == 1);
	uint32_t type_code;
	std::memcpy(&type_code, exported.data() + 1, 4);
	REQUIRE(type_code == static_cast<uint32_t>(WKBGeometryType::PolyhedralSurfaceZ));
}
