#include "catch.hpp"
#include "kernel/geom_model.hpp"
#include "kernel/geom_payload.hpp"
#include <cstring>

using namespace duckdb_3d;

namespace {

//! A 4x3 rectangle at z=5 as a single-ring Polygon (mirrors the SQL fixture).
GeomModel MakeRectanglePolygon() {
	GeomModel model;
	model.type = GeomType::Polygon;
	model.vertices = {{0, 0, 5}, {4, 0, 5}, {4, 3, 5}, {0, 3, 5}};
	model.ring_offsets = {0, 4};
	model.ComputeBBox();
	return model;
}

GeomModel MakeTwoPartMultiLine() {
	GeomModel model;
	model.type = GeomType::MultiLineString;
	model.vertices = {{0, 0, 0}, {3, 4, 12}, {10, 10, 10}, {13, 14, 10}};
	model.part_offsets = {0, 2, 4};
	model.ComputeBBox();
	return model;
}

} // namespace

TEST_CASE("GeomPayload round-trips a polygon", "[geom_payload]") {
	auto model = MakeRectanglePolygon();
	auto bytes = SerializeGeomPayload(model);
	auto restored = DeserializeGeomPayload(bytes.data(), bytes.size());
	REQUIRE(restored.type == GeomType::Polygon);
	REQUIRE(restored.vertices.size() == 4);
	REQUIRE(restored.ring_offsets == model.ring_offsets);
}

TEST_CASE("GeomPayload round-trips a multilinestring", "[geom_payload]") {
	auto model = MakeTwoPartMultiLine();
	auto bytes = SerializeGeomPayload(model);
	auto restored = DeserializeGeomPayload(bytes.data(), bytes.size());
	REQUIRE(restored.type == GeomType::MultiLineString);
	REQUIRE(restored.part_offsets == model.part_offsets);
}

TEST_CASE("GeomPayload rejects truncated payloads", "[geom_payload]") {
	auto bytes = SerializeGeomPayload(MakeRectanglePolygon());
	REQUIRE_THROWS_WITH(DeserializeGeomPayload(bytes.data(), bytes.size() - 5), Catch::Contains("truncated"));
}

TEST_CASE("GeomPayload rejects non-monotone ring offsets", "[geom_payload]") {
	auto model = MakeRectanglePolygon();
	model.ring_offsets = {4, 0};             // front != 0 and decreasing
	model.polygon_triangle_offsets = {0, 0}; // a filled cache: serialise the bad offsets as given
	auto bytes = SerializeGeomPayload(model);
	REQUIRE_THROWS_WITH(DeserializeGeomPayload(bytes.data(), bytes.size()), Catch::Contains("offsets"));
}

TEST_CASE("GeomPayload rejects ring offsets that decrease mid-sequence", "[geom_payload]") {
	// front()==0 and back()==vertex count both hold, so only the monotonicity
	// loop in ValidateOffsets can catch this: {0,3} then a decrease to 2.
	auto model = MakeRectanglePolygon();
	model.ring_offsets = {0, 3, 2, 4};
	model.polygon_triangle_offsets = {0, 0}; // a filled cache: serialise the bad offsets as given
	auto bytes = SerializeGeomPayload(model);
	REQUIRE_THROWS_WITH(DeserializeGeomPayload(bytes.data(), bytes.size()),
	                    Catch::Contains("non-monotonic ring-vertex offsets"));
}

TEST_CASE("GeomPayload rejects ring offsets past the vertex array", "[geom_payload]") {
	auto model = MakeRectanglePolygon();
	model.ring_offsets = {0, 99};            // Geom3DFootprintArea/WKT writer would read vertices[98]
	model.polygon_triangle_offsets = {0, 0}; // a filled cache: serialise the bad offsets as given
	auto bytes = SerializeGeomPayload(model);
	REQUIRE_THROWS_WITH(DeserializeGeomPayload(bytes.data(), bytes.size()), Catch::Contains("offsets"));
}

TEST_CASE("GeomPayload rejects part offsets past the vertex array", "[geom_payload]") {
	auto model = MakeTwoPartMultiLine();
	model.part_offsets = {0, 2, 99}; // Geom3DLength would read vertices[98]
	auto bytes = SerializeGeomPayload(model);
	REQUIRE_THROWS_WITH(DeserializeGeomPayload(bytes.data(), bytes.size()), Catch::Contains("offsets"));
}

TEST_CASE("GeomPayload rejects part offsets past the ring array", "[geom_payload]") {
	GeomModel model;
	model.type = GeomType::MultiPolygon;
	model.vertices = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}};
	model.ring_offsets = {0, 3};
	model.part_offsets = {0, 5};             // WritePolygonWKB would read ring_offsets[5 + 1]
	model.polygon_triangle_offsets = {0, 0}; // a filled cache: serialise the bad offsets as given
	model.ComputeBBox();
	auto bytes = SerializeGeomPayload(model);
	REQUIRE_THROWS_WITH(DeserializeGeomPayload(bytes.data(), bytes.size()), Catch::Contains("offsets"));
}

TEST_CASE("GeomPayload rejects an unknown geometry type code", "[geom_payload]") {
	auto bytes = SerializeGeomPayload(MakeRectanglePolygon());
	uint32_t bogus_type = 999;
	std::memcpy(bytes.data() + 8, &bogus_type, 4); // type sits after magic(4) + version(2+2)
	REQUIRE_THROWS_WITH(DeserializeGeomPayload(bytes.data(), bytes.size()), Catch::Contains("type"));
}

TEST_CASE("GeomPayload is version 2 and carries the CRS", "[geom_payload]") {
	auto model = MakeRectanglePolygon();
	model.crs = "EPSG:28992";
	auto bytes = SerializeGeomPayload(model);
	uint16_t major;
	std::memcpy(&major, bytes.data() + 4, 2);
	REQUIRE(major == 2);
	REQUIRE(DeserializeGeomPayload(bytes.data(), bytes.size()).crs == "EPSG:28992");
	REQUIRE(ReadGeomPayloadHeader(bytes.data(), bytes.size()).crs == "EPSG:28992");
	REQUIRE(ReadGeomPayloadHeader(bytes.data(), bytes.size()).vertex_count == 4);
}

TEST_CASE("GeomPayload refuses a version 1 payload and says to rebuild it", "[geom_payload]") {
	auto bytes = SerializeGeomPayload(MakeRectanglePolygon());
	uint16_t v1 = 1;
	std::memcpy(bytes.data() + 4, &v1, 2);
	REQUIRE_THROWS_WITH(DeserializeGeomPayload(bytes.data(), bytes.size()), Catch::Contains("rebuild"));
	REQUIRE_THROWS_WITH(ReadGeomPayloadHeader(bytes.data(), bytes.size()), Catch::Contains("rebuild"));
}

TEST_CASE("GeomPayload stores each polygon's triangles", "[geom_payload]") {
	auto model = MakeRectanglePolygon();
	REQUIRE(model.polygon_triangle_offsets.empty());
	auto bytes = SerializeGeomPayload(model);
	auto restored = DeserializeGeomPayload(bytes.data(), bytes.size());
	REQUIRE(restored.polygon_triangle_offsets == std::vector<uint32_t> {0, 2});
	REQUIRE(restored.triangle_indices.size() == 6);
	for (auto i : restored.triangle_indices) {
		REQUIRE(i < 4);
	}

	// Lines carry no triangles.
	auto line = DeserializeGeomPayload(SerializeGeomPayload(MakeTwoPartMultiLine()).data(),
	                                   SerializeGeomPayload(MakeTwoPartMultiLine()).size());
	REQUIRE(line.polygon_triangle_offsets.empty());
	REQUIRE(line.triangle_indices.empty());
}

TEST_CASE("GeomPayload gives a polygon without area no triangles", "[geom_payload]") {
	GeomModel model;
	model.type = GeomType::Polygon;
	model.vertices = {{0, 0, 0}, {4, 0, 0}, {6, 0, 0}, {2, 0, 0}};
	model.ring_offsets = {0, 4};
	model.ComputeBBox();
	auto bytes = SerializeGeomPayload(model);
	auto restored = DeserializeGeomPayload(bytes.data(), bytes.size());
	REQUIRE(restored.polygon_triangle_offsets == std::vector<uint32_t> {0, 0});
}

TEST_CASE("GeomPayload refuses a polygon that admits no tiling", "[geom_payload]") {
	GeomModel model;
	model.type = GeomType::Polygon;
	model.vertices = {{0, 0, 0}, {2, 2, 0}, {2, 0, 0}, {0, 2, 0}};
	model.ring_offsets = {0, 4};
	model.ComputeBBox();
	REQUIRE_THROWS_WITH(SerializeGeomPayload(model), Catch::Contains("triangulated"));
}

TEST_CASE("GeomPayload rejects a triangle index past the vertex array", "[geom_payload]") {
	auto model = MakeRectanglePolygon();
	model.polygon_triangle_offsets = {0, 2};
	model.triangle_indices = {0, 1, 2, 0, 2, 9};
	auto bytes = SerializeGeomPayload(model);
	REQUIRE_THROWS_WITH(DeserializeGeomPayload(bytes.data(), bytes.size()), Catch::Contains("triangle"));
}

TEST_CASE("RebuildGeomPayload recomputes a forged triangle cache", "[geom_payload]") {
	// A payload is untrusted once it has been stored as a plain BLOB: its
	// triangles decide distances, so the BLOB -> GEOM_3D cast rebuilds them.
	auto model = MakeRectanglePolygon();
	model.polygon_triangle_offsets = {0, 2};
	model.triangle_indices = {0, 1, 2, 0, 1, 2}; // covers half the rectangle twice
	auto forged = SerializeGeomPayload(model);
	auto rebuilt_bytes = RebuildGeomPayload(forged.data(), forged.size());
	auto rebuilt = DeserializeGeomPayload(rebuilt_bytes.data(), rebuilt_bytes.size());
	auto expected = DeserializeGeomPayload(SerializeGeomPayload(MakeRectanglePolygon()).data(),
	                                       SerializeGeomPayload(MakeRectanglePolygon()).size());
	REQUIRE(rebuilt.triangle_indices == expected.triangle_indices);
	REQUIRE(rebuilt.vertices.size() == 4);
}

TEST_CASE("RebuildGeomPayload refuses a polygon that admits no tiling", "[geom_payload]") {
	GeomModel model;
	model.type = GeomType::Polygon;
	model.vertices = {{0, 0, 0}, {2, 2, 0}, {2, 0, 0}, {0, 2, 0}};
	model.ring_offsets = {0, 4};
	model.ComputeBBox();
	model.polygon_triangle_offsets = {0, 1};
	model.triangle_indices = {0, 1, 2};
	auto forged = SerializeGeomPayload(model);
	REQUIRE_THROWS_WITH(RebuildGeomPayload(forged.data(), forged.size()), Catch::Contains("triangulated"));
}
