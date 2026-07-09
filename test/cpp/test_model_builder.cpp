#include "catch.hpp"
#include "kernel/model_builder.hpp"
#include "kernel/wkb_parser.hpp"
#include "kernel/wkb_export.hpp"
#include "kernel/payload.hpp"
#include <cstring>

using namespace duckdb_3d;

namespace {

//! Helper to build WKB bytes in little-endian
class WKBBuilder {
public:
	std::vector<uint8_t> buffer;
	void WriteByte(uint8_t v) {
		buffer.push_back(v);
	}
	void WriteU32LE(uint32_t v) {
		buffer.push_back(v & 0xFF);
		buffer.push_back((v >> 8) & 0xFF);
		buffer.push_back((v >> 16) & 0xFF);
		buffer.push_back((v >> 24) & 0xFF);
	}
	void WriteF64LE(double v) {
		uint8_t bytes[8];
		std::memcpy(bytes, &v, 8);
		buffer.insert(buffer.end(), bytes, bytes + 8);
	}
	void WriteByteOrder() {
		WriteByte(1);
	}
	void WriteGeometryType(WKBGeometryType type) {
		WriteU32LE(static_cast<uint32_t>(type));
	}
	void WriteRing(const std::vector<Vertex3D> &pts) {
		uint32_t count = static_cast<uint32_t>(pts.size()) + 1; // +1 for closing vertex
		WriteU32LE(count);
		for (auto &p : pts) {
			WriteF64LE(p.x);
			WriteF64LE(p.y);
			WriteF64LE(p.z);
		}
		WriteF64LE(pts[0].x);
		WriteF64LE(pts[0].y);
		WriteF64LE(pts[0].z);
	}
};

std::vector<uint8_t> MakeTetrahedronWKB() {
	WKBBuilder b;
	b.WriteByteOrder();
	b.WriteGeometryType(WKBGeometryType::PolyhedralSurfaceZ);
	b.WriteU32LE(4); // 4 triangular faces

	// Use exact shared vertices so dedup works
	Vertex3D v0 = {0, 0, 0}, v1 = {1, 0, 0}, v2 = {0.5, 1, 0}, v3 = {0.5, 0.5, 1};

	auto writeFace = [&](Vertex3D a, Vertex3D vb, Vertex3D c) {
		b.WriteByteOrder();
		b.WriteGeometryType(WKBGeometryType::PolygonZ);
		b.WriteU32LE(1); // 1 ring
		b.WriteRing({a, vb, c});
	};

	writeFace(v0, v2, v1); // bottom
	writeFace(v0, v1, v3); // front
	writeFace(v1, v2, v3); // right
	writeFace(v2, v0, v3); // left

	return b.buffer;
}

std::vector<uint8_t> MakeUnitCubeWKB() {
	WKBBuilder b;
	b.WriteByteOrder();
	b.WriteGeometryType(WKBGeometryType::PolyhedralSurfaceZ);
	b.WriteU32LE(6);

	Vertex3D v000 = {0, 0, 0}, v100 = {1, 0, 0}, v110 = {1, 1, 0}, v010 = {0, 1, 0};
	Vertex3D v001 = {0, 0, 1}, v101 = {1, 0, 1}, v111 = {1, 1, 1}, v011 = {0, 1, 1};

	auto writeFace = [&](const std::vector<Vertex3D> &pts) {
		b.WriteByteOrder();
		b.WriteGeometryType(WKBGeometryType::PolygonZ);
		b.WriteU32LE(1);
		b.WriteRing(pts);
	};

	writeFace({v000, v100, v110, v010}); // bottom
	writeFace({v001, v011, v111, v101}); // top
	writeFace({v000, v001, v101, v100}); // front
	writeFace({v010, v110, v111, v011}); // back
	writeFace({v000, v010, v011, v001}); // left
	writeFace({v100, v101, v111, v110}); // right

	return b.buffer;
}

} // anonymous namespace

TEST_CASE("BuildSolidModel from tetrahedron WKB", "[model_builder]") {
	auto wkb = MakeTetrahedronWKB();
	auto surfaces = ParseWKB(wkb.data(), wkb.size());
	auto model = BuildSolidModel(surfaces);

	REQUIRE(model.SolidCount() == 1);
	REQUIRE(model.ShellCount() == 1);
	REQUIRE(model.FaceCount() == 4);
	REQUIRE(model.RingCount() == 4);
	// Tetrahedron has 4 unique vertices after dedup
	REQUIRE(model.vertices.size() == 4);
	// Each ring has 3 indices
	REQUIRE(model.ring_vertex_indices.size() == 12);
}

TEST_CASE("BuildSolidModel from unit cube WKB", "[model_builder]") {
	auto wkb = MakeUnitCubeWKB();
	auto surfaces = ParseWKB(wkb.data(), wkb.size());
	auto model = BuildSolidModel(surfaces);

	REQUIRE(model.SolidCount() == 1);
	REQUIRE(model.ShellCount() == 1);
	REQUIRE(model.FaceCount() == 6);
	REQUIRE(model.RingCount() == 6);
	// Cube has 8 unique vertices
	REQUIRE(model.vertices.size() == 8);
	// 6 faces * 4 vertices per face = 24 ring vertex indices
	REQUIRE(model.ring_vertex_indices.size() == 24);
	// BBox check
	REQUIRE(model.bbox.min_x == Approx(0.0));
	REQUIRE(model.bbox.min_y == Approx(0.0));
	REQUIRE(model.bbox.min_z == Approx(0.0));
	REQUIRE(model.bbox.max_x == Approx(1.0));
	REQUIRE(model.bbox.max_y == Approx(1.0));
	REQUIRE(model.bbox.max_z == Approx(1.0));
}

TEST_CASE("BuildSolidModel from GeometryCollection creates multi-solid", "[model_builder]") {
	auto tri_wkb = MakeTetrahedronWKB();
	// Build a GeometryCollection Z with 2 copies
	WKBBuilder b;
	b.WriteByteOrder();
	b.WriteGeometryType(WKBGeometryType::GeometryCollectionZ);
	b.WriteU32LE(2);
	b.buffer.insert(b.buffer.end(), tri_wkb.begin(), tri_wkb.end());
	b.buffer.insert(b.buffer.end(), tri_wkb.begin(), tri_wkb.end());

	auto surfaces = ParseWKB(b.buffer.data(), b.buffer.size());
	auto model = BuildSolidModel(surfaces);

	REQUIRE(model.SolidCount() == 2);
	REQUIRE(model.ShellCount() == 2);
	REQUIRE(model.FaceCount() == 8); // 4 + 4
	// Vertices are shared across solids if identical
	REQUIRE(model.vertices.size() == 4); // same 4 vertices, deduplicated globally
}

TEST_CASE("BuildSolidModel removes duplicate consecutive ring vertices", "[model_builder]") {
	// Build a WKB with a ring that has consecutive duplicate vertices
	WKBBuilder b;
	b.WriteByteOrder();
	b.WriteGeometryType(WKBGeometryType::PolyhedralSurfaceZ);
	b.WriteU32LE(1); // 1 polygon
	// Nested WKBPolygon Z header
	b.WriteByteOrder();
	b.WriteGeometryType(WKBGeometryType::PolygonZ);
	b.WriteU32LE(1); // 1 ring
	// Ring with 6 points: 0,0,0 -> 1,0,0 -> 1,0,0 (dup) -> 0,1,0 -> 0,0,0 (closing)
	b.WriteU32LE(5);
	b.WriteF64LE(0);
	b.WriteF64LE(0);
	b.WriteF64LE(0);
	b.WriteF64LE(1);
	b.WriteF64LE(0);
	b.WriteF64LE(0);
	b.WriteF64LE(1);
	b.WriteF64LE(0);
	b.WriteF64LE(0); // consecutive dup
	b.WriteF64LE(0);
	b.WriteF64LE(1);
	b.WriteF64LE(0);
	b.WriteF64LE(0);
	b.WriteF64LE(0);
	b.WriteF64LE(0); // closing

	auto surfaces = ParseWKB(b.buffer.data(), b.buffer.size());
	auto model = BuildSolidModel(surfaces);

	REQUIRE(model.FaceCount() == 1);
	// Ring should have 3 unique vertices (dup removed)
	uint32_t ring_start = model.ring_vertex_offsets[0];
	uint32_t ring_end = model.ring_vertex_offsets[1];
	REQUIRE(ring_end - ring_start == 3);
}

TEST_CASE("WKB round-trip: tetrahedron", "[wkb_export]") {
	auto wkb_in = MakeTetrahedronWKB();
	auto surfaces = ParseWKB(wkb_in.data(), wkb_in.size());
	auto model = BuildSolidModel(surfaces);
	auto wkb_out = ExportWKB(model);

	// Re-parse the exported WKB
	auto surfaces2 = ParseWKB(wkb_out.data(), wkb_out.size());
	auto model2 = BuildSolidModel(surfaces2);

	REQUIRE(model2.SolidCount() == model.SolidCount());
	REQUIRE(model2.ShellCount() == model.ShellCount());
	REQUIRE(model2.FaceCount() == model.FaceCount());
	REQUIRE(model2.vertices.size() == model.vertices.size());
	REQUIRE(model2.ring_vertex_indices.size() == model.ring_vertex_indices.size());
}

TEST_CASE("WKB round-trip: unit cube", "[wkb_export]") {
	auto wkb_in = MakeUnitCubeWKB();
	auto surfaces = ParseWKB(wkb_in.data(), wkb_in.size());
	auto model = BuildSolidModel(surfaces);
	auto wkb_out = ExportWKB(model);

	auto surfaces2 = ParseWKB(wkb_out.data(), wkb_out.size());
	auto model2 = BuildSolidModel(surfaces2);

	REQUIRE(model2.SolidCount() == 1);
	REQUIRE(model2.FaceCount() == 6);
	REQUIRE(model2.vertices.size() == 8);
}

TEST_CASE("WKB export multi-solid produces GeometryCollection Z", "[wkb_export]") {
	auto tri_wkb = MakeTetrahedronWKB();
	WKBBuilder b;
	b.WriteByteOrder();
	b.WriteGeometryType(WKBGeometryType::GeometryCollectionZ);
	b.WriteU32LE(2);
	b.buffer.insert(b.buffer.end(), tri_wkb.begin(), tri_wkb.end());
	b.buffer.insert(b.buffer.end(), tri_wkb.begin(), tri_wkb.end());

	auto surfaces = ParseWKB(b.buffer.data(), b.buffer.size());
	auto model = BuildSolidModel(surfaces);
	REQUIRE(model.SolidCount() == 2);

	auto wkb_out = ExportWKB(model);

	// Verify output is a GeometryCollection Z
	REQUIRE(wkb_out.size() > 5);
	REQUIRE(wkb_out[0] == 1); // little-endian
	uint32_t type;
	std::memcpy(&type, wkb_out.data() + 1, 4);
	REQUIRE(type == static_cast<uint32_t>(WKBGeometryType::GeometryCollectionZ));

	// Re-parse and verify
	auto surfaces2 = ParseWKB(wkb_out.data(), wkb_out.size());
	auto model2 = BuildSolidModel(surfaces2);
	REQUIRE(model2.SolidCount() == 2);
	REQUIRE(model2.FaceCount() == 8);
}

TEST_CASE("Full payload round-trip: WKB -> SolidModel -> Payload -> SolidModel -> WKB", "[roundtrip]") {
	auto wkb_in = MakeUnitCubeWKB();
	auto surfaces = ParseWKB(wkb_in.data(), wkb_in.size());
	auto model = BuildSolidModel(surfaces);

	// Serialize to payload and back
	auto payload = SerializePayload(model);
	auto model2 = DeserializePayload(payload.data(), payload.size());

	// Export back to WKB
	auto wkb_out = ExportWKB(model2);
	auto surfaces2 = ParseWKB(wkb_out.data(), wkb_out.size());
	auto model3 = BuildSolidModel(surfaces2);

	REQUIRE(model3.SolidCount() == 1);
	REQUIRE(model3.FaceCount() == 6);
	REQUIRE(model3.vertices.size() == 8);
}
