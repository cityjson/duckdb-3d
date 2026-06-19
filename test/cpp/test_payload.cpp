#include "catch.hpp"
#include "kernel/payload.hpp"
#include "kernel/solid_model.hpp"
#include <cstring>

using namespace duckdb_3d;

namespace {

//! Build a minimal tetrahedron model for testing.
//! 4 vertices, 1 solid, 1 shell, 4 triangular faces, 4 rings (one per face),
//! each ring has 3 vertex indices (no WKB closing vertex duplication in canonical form).
SolidModel MakeTetrahedron() {
	SolidModel model;

	// 4 unique vertices of a regular-ish tetrahedron
	model.vertices = {
	    {0.0, 0.0, 0.0}, // 0
	    {1.0, 0.0, 0.0}, // 1
	    {0.5, 1.0, 0.0}, // 2
	    {0.5, 0.5, 1.0}, // 3
	};

	// 1 solid → 1 shell
	model.solid_shell_offsets = {0, 1};
	// 1 shell → 4 faces
	model.shell_face_offsets = {0, 4};
	// 4 faces → 1 ring each
	model.face_ring_offsets = {0, 1, 2, 3, 4};
	// 4 rings → 3 vertex indices each
	model.ring_vertex_offsets = {0, 3, 6, 9, 12};
	// Ring vertex indices (CCW winding when viewed from outside)
	model.ring_vertex_indices = {
	    0, 2, 1, // face 0: bottom
	    0, 1, 3, // face 1
	    1, 2, 3, // face 2
	    2, 0, 3, // face 3
	};

	// Triangulation cache: each face is already a triangle
	model.face_triangle_offsets = {0, 1, 2, 3, 4};
	model.triangle_vertex_indices = model.ring_vertex_indices;

	model.ComputeBBox();

	// Set validation flags for a valid closed tetrahedron
	model.validation.is_closed = true;
	model.validation.is_manifold = true;
	model.validation.is_oriented = true;
	model.validation.is_valid = true;

	return model;
}

} // anonymous namespace

TEST_CASE("Payload header magic and version", "[payload]") {
	SolidModel model = MakeTetrahedron();
	auto bytes = SerializePayload(model);

	// Check magic bytes
	REQUIRE(bytes[0] == 'D');
	REQUIRE(bytes[1] == '3');
	REQUIRE(bytes[2] == 'D');
	REQUIRE(bytes[3] == 'S');

	// Check version (little-endian u16)
	uint16_t major, minor;
	std::memcpy(&major, bytes.data() + 4, 2);
	std::memcpy(&minor, bytes.data() + 6, 2);
	REQUIRE(major == 1);
	REQUIRE(minor == 0);
}

TEST_CASE("Payload round-trip for tetrahedron", "[payload]") {
	SolidModel original = MakeTetrahedron();
	auto bytes = SerializePayload(original);
	SolidModel restored = DeserializePayload(bytes.data(), bytes.size());

	// Counts
	REQUIRE(restored.SolidCount() == 1);
	REQUIRE(restored.ShellCount() == 1);
	REQUIRE(restored.FaceCount() == 4);
	REQUIRE(restored.RingCount() == 4);
	REQUIRE(restored.TriangleCount() == 4);

	// Vertices
	REQUIRE(restored.vertices.size() == 4);
	for (size_t i = 0; i < 4; i++) {
		REQUIRE(restored.vertices[i].x == original.vertices[i].x);
		REQUIRE(restored.vertices[i].y == original.vertices[i].y);
		REQUIRE(restored.vertices[i].z == original.vertices[i].z);
	}

	// BBox
	REQUIRE(restored.bbox.min_x == Approx(0.0));
	REQUIRE(restored.bbox.min_y == Approx(0.0));
	REQUIRE(restored.bbox.min_z == Approx(0.0));
	REQUIRE(restored.bbox.max_x == Approx(1.0));
	REQUIRE(restored.bbox.max_y == Approx(1.0));
	REQUIRE(restored.bbox.max_z == Approx(1.0));

	// Offset arrays
	REQUIRE(restored.solid_shell_offsets == original.solid_shell_offsets);
	REQUIRE(restored.shell_face_offsets == original.shell_face_offsets);
	REQUIRE(restored.face_ring_offsets == original.face_ring_offsets);
	REQUIRE(restored.ring_vertex_offsets == original.ring_vertex_offsets);
	REQUIRE(restored.face_triangle_offsets == original.face_triangle_offsets);

	// Ring vertex indices
	REQUIRE(restored.ring_vertex_indices == original.ring_vertex_indices);

	// Triangle vertex indices
	REQUIRE(restored.triangle_vertex_indices == original.triangle_vertex_indices);

	// Validation cache
	REQUIRE(restored.validation.is_closed == true);
	REQUIRE(restored.validation.is_manifold == true);
	REQUIRE(restored.validation.is_oriented == true);
	REQUIRE(restored.validation.is_valid == true);
	REQUIRE(restored.validation.open_edge_count == 0);
	REQUIRE(restored.validation.non_manifold_edge_count == 0);
	REQUIRE(restored.validation.degenerate_face_count == 0);
	REQUIRE(restored.validation.orientation_error_count == 0);
}

TEST_CASE("Payload rejects invalid magic", "[payload]") {
	SolidModel model = MakeTetrahedron();
	auto bytes = SerializePayload(model);
	bytes[0] = 'X'; // corrupt magic
	REQUIRE_THROWS_WITH(DeserializePayload(bytes.data(), bytes.size()), Catch::Contains("invalid magic"));
}

TEST_CASE("Payload rejects unsupported major version", "[payload]") {
	SolidModel model = MakeTetrahedron();
	auto bytes = SerializePayload(model);
	// Overwrite major version to 99
	uint16_t bad_major = 99;
	std::memcpy(bytes.data() + 4, &bad_major, 2);
	REQUIRE_THROWS_WITH(DeserializePayload(bytes.data(), bytes.size()), Catch::Contains("unsupported major version"));
}

TEST_CASE("Payload rejects truncated data", "[payload]") {
	SolidModel model = MakeTetrahedron();
	auto bytes = SerializePayload(model);
	// Truncate to just the header
	REQUIRE_THROWS_WITH(DeserializePayload(bytes.data(), 10), Catch::Contains("truncated"));
}

TEST_CASE("Payload rejects inconsistent offset arrays", "[payload]") {
	SolidModel model = MakeTetrahedron();
	model.shell_face_offsets = {0, 3};

	auto bytes = SerializePayload(model);
	REQUIRE_THROWS_WITH(DeserializePayload(bytes.data(), bytes.size()), Catch::Contains("offset"));
}

TEST_CASE("Payload rejects out-of-range vertex indices", "[payload]") {
	SolidModel model = MakeTetrahedron();
	model.ring_vertex_indices[0] = 99;

	auto bytes = SerializePayload(model);
	REQUIRE_THROWS_WITH(DeserializePayload(bytes.data(), bytes.size()),
	                    Catch::Contains("vertex index"));
}

TEST_CASE("ReadSolidPayloadHeader matches full deserialisation", "[payload]") {
	SolidModel model = MakeTetrahedron();
	auto bytes = SerializePayload(model);
	auto hdr = ReadSolidPayloadHeader(bytes.data(), bytes.size());

	// Counts come from the fixed front header; no body parsing.
	REQUIRE(hdr.vertex_count == 4);
	REQUIRE(hdr.solid_count == 1);
	REQUIRE(hdr.shell_count == 1);
	REQUIRE(hdr.face_count == 4);
	REQUIRE(hdr.ring_count == 4);
	REQUIRE(hdr.triangle_count == 4);

	// BBox matches.
	REQUIRE(hdr.bbox.min_x == Approx(0.0));
	REQUIRE(hdr.bbox.max_x == Approx(1.0));
	REQUIRE(hdr.bbox.max_z == Approx(1.0));

	// Validation cache comes from the trailing summary block.
	REQUIRE(hdr.validation.is_closed == true);
	REQUIRE(hdr.validation.is_manifold == true);
	REQUIRE(hdr.validation.is_oriented == true);
	REQUIRE(hdr.validation.is_valid == true);
	REQUIRE(hdr.validation.open_edge_count == 0);
}

TEST_CASE("ReadSolidPayloadHeader preserves a non-default validation cache", "[payload]") {
	SolidModel model = MakeTetrahedron();
	model.validation.is_closed = false;
	model.validation.is_valid = false;
	model.validation.open_edge_count = 3;
	auto bytes = SerializePayload(model);
	auto hdr = ReadSolidPayloadHeader(bytes.data(), bytes.size());
	REQUIRE(hdr.validation.is_closed == false);
	REQUIRE(hdr.validation.is_valid == false);
	REQUIRE(hdr.validation.open_edge_count == 3);
}

TEST_CASE("ReadSolidPayloadHeader rejects invalid magic", "[payload]") {
	SolidModel model = MakeTetrahedron();
	auto bytes = SerializePayload(model);
	bytes[0] = 'X';
	REQUIRE_THROWS_WITH(ReadSolidPayloadHeader(bytes.data(), bytes.size()), Catch::Contains("magic"));
}

TEST_CASE("Payload rejects vertex_count exceeding payload size", "[payload]") {
	SolidModel model = MakeTetrahedron();
	auto bytes = SerializePayload(model);
	// Header layout: magic(4) + major(2) + minor(2) + flags(4) = 12, then
	// vertex_count at offset 12. Patch it to a value whose vertex array
	// (count * 24 bytes) cannot fit in the remaining payload. Kept small in
	// absolute terms so the test never triggers a large allocation: the point
	// is that the reader rejects it *before* resizing, not after a truncated read.
	uint32_t bogus_count = 100000;
	std::memcpy(bytes.data() + 12, &bogus_count, 4);
	REQUIRE_THROWS_WITH(DeserializePayload(bytes.data(), bytes.size()), Catch::Contains("exceed"));
}

TEST_CASE("Payload rejects oversized offset-array counts", "[payload]") {
	SolidModel model = MakeTetrahedron();
	auto bytes = SerializePayload(model);
	// solid_count immediately follows vertex_count (offset 16). A huge solid
	// count would size solid_shell_offsets (count+1 uint32) beyond the payload.
	uint32_t bogus_count = 100000;
	std::memcpy(bytes.data() + 16, &bogus_count, 4);
	REQUIRE_THROWS_WITH(DeserializePayload(bytes.data(), bytes.size()), Catch::Contains("exceed"));
}

TEST_CASE("SolidModel ComputeBBox", "[solid_model]") {
	SolidModel model;
	model.vertices = {{-1.0, -2.0, -3.0}, {4.0, 5.0, 6.0}, {0.0, 0.0, 0.0}};
	model.ComputeBBox();
	REQUIRE(model.bbox.min_x == -1.0);
	REQUIRE(model.bbox.min_y == -2.0);
	REQUIRE(model.bbox.min_z == -3.0);
	REQUIRE(model.bbox.max_x == 4.0);
	REQUIRE(model.bbox.max_y == 5.0);
	REQUIRE(model.bbox.max_z == 6.0);
}

TEST_CASE("SolidModel ComputeBBox empty", "[solid_model]") {
	SolidModel model;
	model.ComputeBBox();
	REQUIRE(model.bbox.min_x == 0.0);
	REQUIRE(model.bbox.max_x == 0.0);
}
