#include "catch.hpp"
#include "kernel/arrow_native_import.hpp"

using namespace duckdb_3d;

// Arrow-native ingestion (arrow-native-type branch): boundaries/vertices arrive
// as DuckDB nested LIST/STRUCT Vectors in three_d_extension.cpp, which flattens
// them into the plain ArrowNativeBoundaries CSR form below before calling into
// this DuckDB-free kernel — these tests exercise only that flattened form, no
// DuckDB dependency needed (test/cpp stays fast/no-DuckDB per this repo's
// existing architecture).

TEST_CASE("BuildSolidModelFromArrowNative reconstructs a two-shell solid", "[arrow_native_import]") {
	// 1 solid, 2 shells, 1 triangular face each, sharing a 6-vertex pool.
	ArrowNativeBoundaries boundaries;
	boundaries.solid_shell_offsets = {0, 2};    // 1 solid: 2 shells
	boundaries.shell_face_offsets = {0, 1, 2};  // shell 0: 1 face, shell 1: 1 face
	boundaries.face_ring_offsets = {0, 1, 2};   // face 0: 1 ring, face 1: 1 ring
	boundaries.ring_vertex_offsets = {0, 3, 6}; // ring 0: 3 indices, ring 1: 3 indices
	boundaries.ring_vertex_indices = {0, 1, 2, 3, 4, 5};

	std::vector<Vertex3D> vertices = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}, {0, 0, 1}, {1, 0, 1}, {0, 1, 1}};

	auto model = BuildSolidModelFromArrowNative(boundaries, vertices);
	REQUIRE(model.vertices.size() == 6);
	REQUIRE(model.SolidCount() == 1);
	REQUIRE(model.ShellCount() == 2);
	REQUIRE(model.FaceCount() == 2);
}

TEST_CASE("BuildSolidModelFromArrowNative rejects an out-of-range vertex-pool index", "[arrow_native_import]") {
	ArrowNativeBoundaries boundaries;
	boundaries.solid_shell_offsets = {0, 1};
	boundaries.shell_face_offsets = {0, 1};
	boundaries.face_ring_offsets = {0, 1};
	boundaries.ring_vertex_offsets = {0, 3};
	boundaries.ring_vertex_indices = {0, 1, 3}; // only 3 vertices (indices 0..2) exist

	std::vector<Vertex3D> vertices = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}};

	REQUIRE_THROWS_WITH(BuildSolidModelFromArrowNative(boundaries, vertices), Catch::Contains("out of range"));
}

TEST_CASE("BuildSolidModelFromArrowNative deduplicates coordinate-equal vertices at distinct pool indices",
          "[arrow_native_import]") {
	// A buggy/naive writer emits the same point {0,0,1} twice, at indices 3
	// and 6 — a real triangulated tetrahedron's two faces sharing that apex,
	// each referencing "their own" copy instead of a shared index. Without
	// dedup, ValidateSolidModel would see 2 distinct vertex indices for what
	// is topologically one shared vertex, and misreport an open edge.
	ArrowNativeBoundaries boundaries;
	boundaries.solid_shell_offsets = {0, 1};
	boundaries.shell_face_offsets = {0, 4};
	boundaries.face_ring_offsets = {0, 1, 2, 3, 4};
	boundaries.ring_vertex_offsets = {0, 3, 6, 9, 12};
	boundaries.ring_vertex_indices = {
	    0, 2, 1, // base (reversed for outward winding)
	    0, 1, 3, // side using apex copy #1 (index 3)
	    1, 2, 6, // side using apex copy #2 (index 6) — same coordinates as index 3
	    2, 0, 3, // side using apex copy #1 again
	};

	std::vector<Vertex3D> vertices = {
	    {0, 0, 0}, {2, 0, 0}, {1, 2, 0}, // base triangle
	    {1, 1, 2},                       // apex, copy #1 (index 3)
	    {0, 0, 0}, {2, 0, 0},            // unused filler to make index 6 land on the duplicate apex
	    {1, 1, 2},                       // apex, copy #2 (index 6) — coordinate-identical to index 3
	};

	auto model = BuildSolidModelFromArrowNative(boundaries, vertices);
	REQUIRE(model.vertices.size() == 4); // deduped: 3 base + 1 apex, not 7
	REQUIRE(model.ShellCount() == 1);
	REQUIRE(model.FaceCount() == 4);
	REQUIRE(model.validation.is_closed);
	REQUIRE(model.validation.is_manifold);
}

TEST_CASE("BuildSolidModelFromArrowNative flags a degenerate (zero-length) ring, not a crash",
          "[arrow_native_import]") {
	// A face whose only ring has 0 indices (offset delta 0) — passes straight
	// through to the same TriangulateSolidModel/ValidateSolidModel the WKB
	// path already uses, so it must be flagged as degenerate exactly like a
	// degenerate WKB ring is (DESIGN_DOC.md §9.4), not throw or crash here.
	ArrowNativeBoundaries boundaries;
	boundaries.solid_shell_offsets = {0, 1};
	boundaries.shell_face_offsets = {0, 1};
	boundaries.face_ring_offsets = {0, 1};
	boundaries.ring_vertex_offsets = {0, 0}; // one ring, zero vertices
	boundaries.ring_vertex_indices = {};

	std::vector<Vertex3D> vertices = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}};

	auto model = BuildSolidModelFromArrowNative(boundaries, vertices);
	REQUIRE(model.FaceCount() == 1);
	REQUIRE(model.validation.degenerate_face_count >= 1);
}

TEST_CASE("BuildGeomModelFromArrowNative strips padding and dereferences into inline coordinates",
          "[arrow_native_import]") {
	// Same physical shape as a Solid with 1 shell, 1 face, but semantically a
	// MultiSurface (single triangular surface) — the padding dimensions
	// (solid-count 1, shell-count 1) carry no meaning here (design doc).
	ArrowNativeBoundaries boundaries;
	boundaries.solid_shell_offsets = {0, 1}; // padding: exactly 1 solid
	boundaries.shell_face_offsets = {0, 1};  // padding: exactly 1 shell
	boundaries.face_ring_offsets = {0, 1};   // 1 face: 1 ring
	boundaries.ring_vertex_offsets = {0, 3}; // 1 ring: 3 indices
	boundaries.ring_vertex_indices = {0, 1, 2};

	std::vector<Vertex3D> vertices = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}};

	auto model = BuildGeomModelFromArrowNative(boundaries, vertices);
	REQUIRE(model.type == GeomType::MultiPolygon);
	REQUIRE(model.vertices.size() == 3);     // GeomModel is NOT index-based — inline coordinates
	REQUIRE(model.part_offsets.size() == 2); // 1 part (this face) -> part_offsets = [0, 1]
	REQUIRE(model.ring_offsets.size() == 2); // 1 ring -> ring_offsets = [0, 3]
	REQUIRE(model.vertices[0] == Vertex3D {0, 0, 0});
	REQUIRE(model.vertices[1] == Vertex3D {1, 0, 0});
	REQUIRE(model.vertices[2] == Vertex3D {0, 1, 0});
}

TEST_CASE("BuildGeomModelFromArrowNative rejects a non-padded (real multi-shell) value", "[arrow_native_import]") {
	ArrowNativeBoundaries boundaries;
	boundaries.solid_shell_offsets = {0, 2}; // 2 shells -> not a padded surface value
	boundaries.shell_face_offsets = {0, 1, 2};
	boundaries.face_ring_offsets = {0, 1, 2};
	boundaries.ring_vertex_offsets = {0, 3, 6};
	boundaries.ring_vertex_indices = {0, 1, 2, 0, 1, 2};

	std::vector<Vertex3D> vertices = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}};

	REQUIRE_THROWS_WITH(BuildGeomModelFromArrowNative(boundaries, vertices), Catch::Contains("padding"));
}

TEST_CASE("BuildGeomModelFromArrowNative rejects an out-of-range vertex-pool index", "[arrow_native_import]") {
	ArrowNativeBoundaries boundaries;
	boundaries.solid_shell_offsets = {0, 1};
	boundaries.shell_face_offsets = {0, 1};
	boundaries.face_ring_offsets = {0, 1};
	boundaries.ring_vertex_offsets = {0, 3};
	boundaries.ring_vertex_indices = {0, 1, 3}; // only 3 vertices (indices 0..2) exist

	std::vector<Vertex3D> vertices = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}};

	REQUIRE_THROWS_WITH(BuildGeomModelFromArrowNative(boundaries, vertices), Catch::Contains("out of range"));
}
