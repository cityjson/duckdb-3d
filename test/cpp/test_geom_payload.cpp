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
	model.ring_offsets = {4, 0}; // front != 0 and decreasing
	auto bytes = SerializeGeomPayload(model);
	REQUIRE_THROWS_WITH(DeserializeGeomPayload(bytes.data(), bytes.size()), Catch::Contains("offsets"));
}

TEST_CASE("GeomPayload rejects ring offsets that decrease mid-sequence", "[geom_payload]") {
	// front()==0 and back()==vertex count both hold, so only the monotonicity
	// loop in ValidateGeomOffsets can catch this: {0,3} then a decrease to 2.
	auto model = MakeRectanglePolygon();
	model.ring_offsets = {0, 3, 2, 4};
	auto bytes = SerializeGeomPayload(model);
	REQUIRE_THROWS_WITH(DeserializeGeomPayload(bytes.data(), bytes.size()),
	                     Catch::Contains("non-monotonic ring-vertex offsets"));
}

TEST_CASE("GeomPayload rejects ring offsets past the vertex array", "[geom_payload]") {
	auto model = MakeRectanglePolygon();
	model.ring_offsets = {0, 99}; // Geom3DFootprintArea/WKT writer would read vertices[98]
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
	model.part_offsets = {0, 5}; // WritePolygonWKB would read ring_offsets[5 + 1]
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
