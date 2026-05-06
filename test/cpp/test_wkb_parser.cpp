#include "catch.hpp"
#include "kernel/wkb_parser.hpp"
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
		WriteByte(1); // little-endian
	}
	void WriteGeometryType(WKBGeometryType type) {
		WriteU32LE(static_cast<uint32_t>(type));
	}
	//! Write a polygon ring: count + XYZ coordinates (with WKB closing vertex)
	void WriteRing(const std::vector<Vertex3D> &pts, bool close = true) {
		uint32_t count = static_cast<uint32_t>(pts.size()) + (close ? 1 : 0);
		WriteU32LE(count);
		for (auto &p : pts) {
			WriteF64LE(p.x);
			WriteF64LE(p.y);
			WriteF64LE(p.z);
		}
		if (close) {
			WriteF64LE(pts[0].x);
			WriteF64LE(pts[0].y);
			WriteF64LE(pts[0].z);
		}
	}
};

//! Build a WKB PolyhedralSurface Z representing a unit cube with 6 quad faces.
//! Vertices: (0,0,0) to (1,1,1)
std::vector<uint8_t> MakeUnitCubeWKB() {
	WKBBuilder b;
	b.WriteByteOrder();
	b.WriteGeometryType(WKBGeometryType::PolyhedralSurfaceZ);
	b.WriteU32LE(6); // 6 polygons

	// Face 0: bottom (z=0), CCW from outside (looking down) = CW from above
	std::vector<Vertex3D> bottom = {{0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0}};
	// Face 1: top (z=1), CCW from outside (looking up)
	std::vector<Vertex3D> top = {{0, 0, 1}, {0, 1, 1}, {1, 1, 1}, {1, 0, 1}};
	// Face 2: front (y=0)
	std::vector<Vertex3D> front = {{0, 0, 0}, {0, 0, 1}, {1, 0, 1}, {1, 0, 0}};
	// Face 3: back (y=1)
	std::vector<Vertex3D> back = {{0, 1, 0}, {1, 1, 0}, {1, 1, 1}, {0, 1, 1}};
	// Face 4: left (x=0)
	std::vector<Vertex3D> left = {{0, 0, 0}, {0, 1, 0}, {0, 1, 1}, {0, 0, 1}};
	// Face 5: right (x=1)
	std::vector<Vertex3D> right = {{1, 0, 0}, {1, 0, 1}, {1, 1, 1}, {1, 1, 0}};

	auto write_polygon = [&](const std::vector<Vertex3D> &ring) {
		b.WriteByteOrder();
		b.WriteGeometryType(WKBGeometryType::PolygonZ);
		b.WriteU32LE(1); // 1 ring per polygon
		b.WriteRing(ring);
	};

	write_polygon(bottom);
	write_polygon(top);
	write_polygon(front);
	write_polygon(back);
	write_polygon(left);
	write_polygon(right);

	return b.buffer;
}

//! Build WKB for a simple triangle PolyhedralSurface Z (1 polygon, 1 ring, 3 vertices)
std::vector<uint8_t> MakeTriangleWKB() {
	WKBBuilder b;
	b.WriteByteOrder();
	b.WriteGeometryType(WKBGeometryType::PolyhedralSurfaceZ);
	b.WriteU32LE(1); // 1 polygon
	// Nested WKBPolygon Z header
	b.WriteByteOrder();
	b.WriteGeometryType(WKBGeometryType::PolygonZ);
	b.WriteU32LE(1); // 1 ring
	b.WriteRing({{0, 0, 0}, {1, 0, 0}, {0, 1, 0}});
	return b.buffer;
}

//! Build WKB GeometryCollection Z containing two PolyhedralSurface Z
std::vector<uint8_t> MakeGeometryCollectionWKB() {
	auto tri = MakeTriangleWKB();

	WKBBuilder b;
	b.WriteByteOrder();
	b.WriteGeometryType(WKBGeometryType::GeometryCollectionZ);
	b.WriteU32LE(2); // 2 children
	// Child 1
	b.buffer.insert(b.buffer.end(), tri.begin(), tri.end());
	// Child 2
	b.buffer.insert(b.buffer.end(), tri.begin(), tri.end());
	return b.buffer;
}

//! Build WKB Polygon Z (unsupported type)
std::vector<uint8_t> MakePolygonZWKB() {
	WKBBuilder b;
	b.WriteByteOrder();
	b.WriteGeometryType(WKBGeometryType::PolygonZ);
	b.WriteU32LE(1); // 1 ring
	b.WriteRing({{0, 0, 0}, {1, 0, 0}, {0, 1, 0}});
	return b.buffer;
}

//! Build WKB TIN Z (unsupported type)
std::vector<uint8_t> MakeTINZWKB() {
	WKBBuilder b;
	b.WriteByteOrder();
	b.WriteGeometryType(WKBGeometryType::TINZ);
	b.WriteU32LE(0); // 0 triangles
	return b.buffer;
}

} // anonymous namespace

TEST_CASE("WKB parse PolyhedralSurface Z - unit cube", "[wkb_parser]") {
	auto wkb = MakeUnitCubeWKB();
	auto surfaces = ParseWKB(wkb.data(), wkb.size());

	REQUIRE(surfaces.size() == 1);
	auto &s = surfaces[0];
	REQUIRE(s.polygon_count == 6);
	REQUIRE(s.polygon_ring_counts.size() == 6);
	for (auto rc : s.polygon_ring_counts) {
		REQUIRE(rc == 1); // each polygon has 1 ring
	}
	// Each quad ring has 4 unique vertices (closing vertex removed)
	REQUIRE(s.ring_vertex_counts.size() == 6);
	for (auto vc : s.ring_vertex_counts) {
		REQUIRE(vc == 4);
	}
	// Total vertices: 6 faces * 5 WKB vertices = 30, but closing removed = 6*4 = 24
	REQUIRE(s.vertices.size() == 24);
}

TEST_CASE("WKB parse PolyhedralSurface Z - single triangle", "[wkb_parser]") {
	auto wkb = MakeTriangleWKB();
	auto surfaces = ParseWKB(wkb.data(), wkb.size());

	REQUIRE(surfaces.size() == 1);
	auto &s = surfaces[0];
	REQUIRE(s.polygon_count == 1);
	REQUIRE(s.ring_vertex_counts[0] == 3);
	REQUIRE(s.vertices.size() == 3);
	REQUIRE(s.vertices[0].x == Approx(0.0));
	REQUIRE(s.vertices[1].x == Approx(1.0));
	REQUIRE(s.vertices[2].y == Approx(1.0));
}

TEST_CASE("WKB parse GeometryCollection Z of PolyhedralSurface Z", "[wkb_parser]") {
	auto wkb = MakeGeometryCollectionWKB();
	auto surfaces = ParseWKB(wkb.data(), wkb.size());

	REQUIRE(surfaces.size() == 2);
	REQUIRE(surfaces[0].polygon_count == 1);
	REQUIRE(surfaces[1].polygon_count == 1);
}

TEST_CASE("WKB rejects unsupported Polygon Z", "[wkb_parser]") {
	auto wkb = MakePolygonZWKB();
	REQUIRE_THROWS_WITH(ParseWKB(wkb.data(), wkb.size()), Catch::Contains("Unsupported"));
}

TEST_CASE("WKB rejects unsupported TIN Z", "[wkb_parser]") {
	auto wkb = MakeTINZWKB();
	REQUIRE_THROWS_WITH(ParseWKB(wkb.data(), wkb.size()), Catch::Contains("Unsupported"));
}

TEST_CASE("WKB rejects empty data", "[wkb_parser]") {
	REQUIRE_THROWS(ParseWKB(nullptr, 0));
}

TEST_CASE("WKB rejects GeometryCollection with non-PolyhedralSurface child", "[wkb_parser]") {
	auto poly = MakePolygonZWKB();
	WKBBuilder b;
	b.WriteByteOrder();
	b.WriteGeometryType(WKBGeometryType::GeometryCollectionZ);
	b.WriteU32LE(1);
	b.buffer.insert(b.buffer.end(), poly.begin(), poly.end());

	REQUIRE_THROWS_WITH(ParseWKB(b.buffer.data(), b.buffer.size()), Catch::Contains("Unsupported"));
}
