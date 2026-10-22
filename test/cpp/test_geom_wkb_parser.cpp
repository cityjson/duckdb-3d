#include "catch.hpp"
#include "kernel/geom_wkb_parser.hpp"
#include <array>
#include <cstring>
#include <limits>

using namespace duckdb_3d;

namespace {

//! Minimal little-endian WKB builder for GEOM_3D parser tests.
class GeomWKBBuilder {
public:
	std::vector<uint8_t> buffer;

	void U8(uint8_t v) {
		buffer.push_back(v);
	}
	void U32(uint32_t v) {
		buffer.push_back(v & 0xFF);
		buffer.push_back((v >> 8) & 0xFF);
		buffer.push_back((v >> 16) & 0xFF);
		buffer.push_back((v >> 24) & 0xFF);
	}
	void F64(double v) {
		uint8_t b[8];
		std::memcpy(b, &v, 8);
		buffer.insert(buffer.end(), b, b + 8);
	}
	void Point(double x, double y, double z) {
		F64(x);
		F64(y);
		F64(z);
	}
	//! Write a ring as count + XYZ points, appending the WKB closing vertex.
	void Ring(const std::vector<std::array<double, 3>> &pts) {
		U32(static_cast<uint32_t>(pts.size()) + 1);
		for (auto &p : pts) {
			Point(p[0], p[1], p[2]);
		}
		Point(pts[0][0], pts[0][1], pts[0][2]);
	}
};

//! Minimal big-endian WKB builder, mirroring GeomWKBBuilder. Used to verify the
//! parser handles byte-order byte 0 (big-endian) for both top-level and child
//! geometries.
class GeomWKBBuilderBE {
public:
	std::vector<uint8_t> buffer;

	void U8(uint8_t v) {
		buffer.push_back(v);
	}
	void U32(uint32_t v) {
		buffer.push_back((v >> 24) & 0xFF);
		buffer.push_back((v >> 16) & 0xFF);
		buffer.push_back((v >> 8) & 0xFF);
		buffer.push_back(v & 0xFF);
	}
	void F64(double v) {
		uint8_t b[8];
		std::memcpy(b, &v, 8);
		for (int i = 7; i >= 0; i--) {
			buffer.push_back(b[i]);
		}
	}
	void Point(double x, double y, double z) {
		F64(x);
		F64(y);
		F64(z);
	}
	void Ring(const std::vector<std::array<double, 3>> &pts) {
		U32(static_cast<uint32_t>(pts.size()) + 1);
		for (auto &p : pts) {
			Point(p[0], p[1], p[2]);
		}
		Point(pts[0][0], pts[0][1], pts[0][2]);
	}
};

} // namespace

TEST_CASE("ParseGeomWKB: big-endian Point Z", "[geom_wkb_parser]") {
	GeomWKBBuilderBE b;
	b.U8(0);     // big-endian
	b.U32(1001); // Point Z
	b.Point(1.0, 2.0, 9.0);

	auto model = ParseGeomWKB(b.buffer.data(), b.buffer.size());

	REQUIRE(model.type == GeomType::Point);
	REQUIRE(model.vertices.size() == 1);
	REQUIRE(model.vertices[0].x == Approx(1.0));
	REQUIRE(model.vertices[0].y == Approx(2.0));
	REQUIRE(model.vertices[0].z == Approx(9.0));
}

TEST_CASE("ParseGeomWKB: big-endian LineString Z", "[geom_wkb_parser]") {
	GeomWKBBuilderBE b;
	b.U8(0);     // big-endian
	b.U32(1002); // LineString Z
	b.U32(3);    // point count
	b.Point(0, 0, 0);
	b.Point(1, 1, 1);
	b.Point(2, 4, 8);

	auto model = ParseGeomWKB(b.buffer.data(), b.buffer.size());

	REQUIRE(model.type == GeomType::LineString);
	REQUIRE(model.vertices.size() == 3);
	REQUIRE(model.vertices[2].y == Approx(4.0));
	REQUIRE(model.bbox.max_z == Approx(8.0));
}

TEST_CASE("ParseGeomWKB: big-endian Polygon Z", "[geom_wkb_parser]") {
	GeomWKBBuilderBE b;
	b.U8(0);     // big-endian
	b.U32(1003); // Polygon Z
	b.U32(1);    // ring count
	b.Ring({{0, 0, 5}, {4, 0, 5}, {4, 3, 5}, {0, 3, 5}});

	auto model = ParseGeomWKB(b.buffer.data(), b.buffer.size());

	REQUIRE(model.type == GeomType::Polygon);
	REQUIRE(model.vertices.size() == 4);
	REQUIRE(model.ring_offsets == std::vector<uint32_t> {0, 4});
	REQUIRE(model.bbox.max_x == Approx(4.0));
	REQUIRE(model.bbox.max_y == Approx(3.0));
	REQUIRE(model.bbox.min_z == Approx(5.0));
}

TEST_CASE("ParseGeomWKB: big-endian PolyhedralSurface Z with big-endian children", "[geom_wkb_parser]") {
	GeomWKBBuilderBE b;
	b.U8(0);
	b.U32(1015); // PolyhedralSurface Z
	b.U32(2);    // 2 faces
	b.U8(0);
	b.U32(1003);
	b.U32(1);
	b.Ring({{0, 0, 0}, {1, 0, 0}, {0, 1, 0}});
	b.U8(0);
	b.U32(1003);
	b.U32(1);
	b.Ring({{0, 0, 0}, {0, 1, 0}, {0, 0, 2}});

	auto model = ParseGeomWKB(b.buffer.data(), b.buffer.size());

	REQUIRE(model.type == GeomType::PolyhedralSurface);
	REQUIRE(model.vertices.size() == 6);
	REQUIRE(model.ring_offsets == std::vector<uint32_t> {0, 3, 6});
	REQUIRE(model.part_offsets == std::vector<uint32_t> {0, 1, 2});
	REQUIRE(model.bbox.max_z == Approx(2.0));
}

TEST_CASE("ParseGeomWKB: Polygon Z single exterior ring", "[geom_wkb_parser]") {
	GeomWKBBuilder b;
	b.U8(1);     // little-endian
	b.U32(1003); // Polygon Z
	b.U32(1);    // ring count
	b.Ring({{0, 0, 5}, {4, 0, 5}, {4, 3, 5}, {0, 3, 5}});

	auto model = ParseGeomWKB(b.buffer.data(), b.buffer.size());

	REQUIRE(model.type == GeomType::Polygon);
	// Closing vertex stripped: 4 distinct vertices.
	REQUIRE(model.vertices.size() == 4);
	REQUIRE(model.ring_offsets == std::vector<uint32_t> {0, 4});
	REQUIRE(model.part_offsets.empty());
	REQUIRE(model.bbox.min_x == 0.0);
	REQUIRE(model.bbox.max_x == 4.0);
	REQUIRE(model.bbox.max_y == 3.0);
	REQUIRE(model.bbox.min_z == 5.0);
	REQUIRE(model.bbox.max_z == 5.0);
}

TEST_CASE("ParseGeomWKB: PolyhedralSurface Z two faces", "[geom_wkb_parser]") {
	GeomWKBBuilder b;
	b.U8(1);
	b.U32(1015); // PolyhedralSurface Z
	b.U32(2);    // 2 faces
	// Face 0
	b.U8(1);
	b.U32(1003);
	b.U32(1);
	b.Ring({{0, 0, 0}, {1, 0, 0}, {0, 1, 0}});
	// Face 1
	b.U8(1);
	b.U32(1003);
	b.U32(1);
	b.Ring({{0, 0, 0}, {0, 1, 0}, {0, 0, 2}});

	auto model = ParseGeomWKB(b.buffer.data(), b.buffer.size());

	REQUIRE(model.type == GeomType::PolyhedralSurface);
	REQUIRE(model.vertices.size() == 6);
	REQUIRE(model.ring_offsets == std::vector<uint32_t> {0, 3, 6});
	REQUIRE(model.part_offsets == std::vector<uint32_t> {0, 1, 2});
	REQUIRE(model.bbox.max_z == 2.0);
}

TEST_CASE("ParseGeomWKB: MultiPolygon Z two square parts", "[geom_wkb_parser]") {
	GeomWKBBuilder b;
	b.U8(1);
	b.U32(1006); // MultiPolygon Z
	b.U32(2);    // 2 polygons
	// Polygon 0: one ring
	b.U8(1);
	b.U32(1003);
	b.U32(1);
	b.Ring({{0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0}});
	// Polygon 1: exterior + hole
	b.U8(1);
	b.U32(1003);
	b.U32(2);
	b.Ring({{0, 0, 7}, {10, 0, 7}, {10, 10, 7}, {0, 10, 7}});
	b.Ring({{2, 2, 7}, {4, 2, 7}, {4, 4, 7}, {2, 4, 7}});

	auto model = ParseGeomWKB(b.buffer.data(), b.buffer.size());

	REQUIRE(model.type == GeomType::MultiPolygon);
	REQUIRE(model.vertices.size() == 12);
	// 3 rings total → ring_offsets has 4 boundaries.
	REQUIRE(model.ring_offsets == std::vector<uint32_t> {0, 4, 8, 12});
	// part_offsets index into ring boundaries: poly 0 = ring [0,1), poly 1 = rings [1,3).
	REQUIRE(model.part_offsets == std::vector<uint32_t> {0, 1, 3});
	REQUIRE(model.bbox.max_z == 7.0);
}

TEST_CASE("ParseGeomWKB: MultiPoint Z", "[geom_wkb_parser]") {
	GeomWKBBuilder b;
	b.U8(1);
	b.U32(1004); // MultiPoint Z
	b.U32(3);    // 3 points
	for (auto &p : std::vector<std::array<double, 3>> {{1, 1, 1}, {2, 2, 2}, {3, 3, 9}}) {
		b.U8(1);
		b.U32(1001); // Point Z
		b.Point(p[0], p[1], p[2]);
	}

	auto model = ParseGeomWKB(b.buffer.data(), b.buffer.size());

	REQUIRE(model.type == GeomType::MultiPoint);
	REQUIRE(model.vertices.size() == 3);
	// One part per point, partitioning the vertex array directly.
	REQUIRE(model.part_offsets == std::vector<uint32_t> {0, 1, 2, 3});
	REQUIRE(model.ring_offsets.empty());
	REQUIRE(model.bbox.max_z == 9.0);
}

TEST_CASE("ParseGeomWKB: Polygon Z with interior ring", "[geom_wkb_parser]") {
	GeomWKBBuilder b;
	b.U8(1);
	b.U32(1003);
	b.U32(2); // exterior + 1 hole
	b.Ring({{0, 0, 0}, {10, 0, 0}, {10, 10, 0}, {0, 10, 0}});
	b.Ring({{2, 2, 0}, {4, 2, 0}, {4, 4, 0}, {2, 4, 0}});

	auto model = ParseGeomWKB(b.buffer.data(), b.buffer.size());

	REQUIRE(model.type == GeomType::Polygon);
	REQUIRE(model.vertices.size() == 8);
	REQUIRE(model.ring_offsets == std::vector<uint32_t> {0, 4, 8});
}

// EWKB (PostGIS's dialect) flags Z with 0x80000000 and an SRID with 0x20000000,
// the SRID following the type code. The SRID becomes the model's CRS as EPSG:n.
TEST_CASE("ParseGeomWKB: EWKB Point Z with an SRID", "[geom_wkb_parser][ewkb]") {
	GeomWKBBuilder b;
	b.U8(1);
	b.U32(0xA0000001); // Point | Z | SRID
	b.U32(28992);
	b.Point(1.0, 2.0, 3.0);

	auto model = ParseGeomWKB(b.buffer.data(), b.buffer.size());

	REQUIRE(model.type == GeomType::Point);
	REQUIRE(model.vertices[0].z == 3.0);
	REQUIRE(model.crs == "EPSG:28992");
}

TEST_CASE("ParseGeomWKB: EWKB MultiPolygon Z, children flagged Z only", "[geom_wkb_parser][ewkb]") {
	GeomWKBBuilder b;
	b.U8(1);
	b.U32(0xA0000006); // MultiPolygon | Z | SRID
	b.U32(7415);
	b.U32(2);
	for (double z : {0.0, 5.0}) {
		b.U8(1);
		b.U32(0x80000003); // Polygon | Z
		b.U32(1);
		b.Ring({{0, 0, z}, {1, 0, z}, {1, 1, z}});
	}

	auto model = ParseGeomWKB(b.buffer.data(), b.buffer.size());

	REQUIRE(model.type == GeomType::MultiPolygon);
	REQUIRE(model.part_offsets == std::vector<uint32_t> {0, 1, 2});
	REQUIRE(model.crs == "EPSG:7415");
}

TEST_CASE("ParseGeomWKB: EWKB without an SRID, or SRID 0, has no CRS", "[geom_wkb_parser][ewkb]") {
	GeomWKBBuilder plain;
	plain.U8(1);
	plain.U32(0x80000002); // LineString | Z
	plain.U32(2);
	plain.Point(0, 0, 0);
	plain.Point(1, 1, 1);
	auto model = ParseGeomWKB(plain.buffer.data(), plain.buffer.size());
	REQUIRE(model.type == GeomType::LineString);
	REQUIRE(model.crs.empty());

	GeomWKBBuilder zero;
	zero.U8(1);
	zero.U32(0xA0000001);
	zero.U32(0);
	zero.Point(0, 0, 0);
	REQUIRE(ParseGeomWKB(zero.buffer.data(), zero.buffer.size()).crs.empty());
}

TEST_CASE("ParseGeomWKB: EWKB refuses M, 2D, and a child SRID that differs", "[geom_wkb_parser][ewkb]") {
	GeomWKBBuilder m;
	m.U8(1);
	m.U32(0xC0000001); // Point | Z | M
	m.Point(0, 0, 0);
	m.F64(0);
	REQUIRE_THROWS_WITH(ParseGeomWKB(m.buffer.data(), m.buffer.size()), Catch::Contains("unsupported"));

	GeomWKBBuilder flat;
	flat.U8(1);
	flat.U32(0x20000001); // Point | SRID, no Z
	flat.U32(28992);
	flat.F64(0);
	flat.F64(0);
	REQUIRE_THROWS_WITH(ParseGeomWKB(flat.buffer.data(), flat.buffer.size()), Catch::Contains("unsupported"));

	GeomWKBBuilder child;
	child.U8(1);
	child.U32(0xA0000004); // MultiPoint | Z | SRID
	child.U32(28992);
	child.U32(1);
	child.U8(1);
	child.U32(0xA0000001);
	child.U32(4326);
	child.Point(0, 0, 0);
	REQUIRE_THROWS_WITH(ParseGeomWKB(child.buffer.data(), child.buffer.size()), Catch::Contains("SRID"));
}

TEST_CASE("ParseGeomWKB: ISO WKB has no CRS", "[geom_wkb_parser][ewkb]") {
	GeomWKBBuilder b;
	b.U8(1);
	b.U32(1001);
	b.Point(1, 2, 3);
	REQUIRE(ParseGeomWKB(b.buffer.data(), b.buffer.size()).crs.empty());
}

TEST_CASE("ParseGeomWKB: refuses a non-finite coordinate", "[geom_wkb_parser]") {
	const double nan = std::numeric_limits<double>::quiet_NaN();
	const double inf = std::numeric_limits<double>::infinity();
	SECTION("NaN in a line") {
		GeomWKBBuilder b;
		b.U8(1);
		b.U32(1002);
		b.U32(2);
		b.Point(nan, 0, 0);
		b.Point(1, 1, 1);
		REQUIRE_THROWS_WITH(ParseGeomWKB(b.buffer.data(), b.buffer.size()), Catch::Contains("finite"));
	}
	SECTION("infinity in a ring") {
		GeomWKBBuilder b;
		b.U8(1);
		b.U32(1003);
		b.U32(1);
		b.Ring({{0, 0, 0}, {-inf, 0, 0}, {1, 1, 0}});
		REQUIRE_THROWS_WITH(ParseGeomWKB(b.buffer.data(), b.buffer.size()), Catch::Contains("finite"));
	}
	SECTION("a point NaN in one ordinate only") {
		GeomWKBBuilder b;
		b.U8(1);
		b.U32(1001);
		b.Point(1, 2, nan);
		REQUIRE_THROWS_WITH(ParseGeomWKB(b.buffer.data(), b.buffer.size()), Catch::Contains("finite"));
	}
	SECTION("POINT EMPTY, all three NaN, is still read") {
		GeomWKBBuilder b;
		b.U8(1);
		b.U32(1001);
		b.Point(nan, nan, nan);
		REQUIRE(ParseGeomWKB(b.buffer.data(), b.buffer.size()).vertices.size() == 1);
	}
}
