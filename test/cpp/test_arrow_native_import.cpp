#include "catch.hpp"
#include "kernel/arrow_native_import.hpp"
#include "kernel/measurements.hpp"
#include "kernel/metadata_parser.hpp"
#include "kernel/model_builder.hpp"
#include "kernel/triangulation.hpp"
#include "kernel/validation.hpp"
#include "kernel/wkb_parser.hpp"
#include <algorithm>
#include <cstring>

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

TEST_CASE("BuildSolidModelFromArrowNative skips a consecutive-duplicate compact index within a ring",
          "[arrow_native_import]") {
	// A validated closed/manifold tetrahedron (the same face set
	// test_metadata.cpp's MakeTwoShellWKB uses: A=v0,B=v1,C=v2,D=v3, faces
	// ACB/ABD/BCD/CAD), but face0's ring carries an extra raw index (4) that
	// is coordinate-identical to C (index 2) and placed immediately adjacent
	// to it: A,C,C',B instead of A,C,B. Without collapsing this into A,C,B
	// (mirroring model_builder.cpp's IsConsecutiveDuplicate for the WKB
	// path), the C-to-C' "edge" is a zero-length self-loop with no twin
	// anywhere else in the model, and the solid misreports as open.
	ArrowNativeBoundaries boundaries;
	boundaries.solid_shell_offsets = {0, 1};
	boundaries.shell_face_offsets = {0, 4};
	boundaries.face_ring_offsets = {0, 1, 2, 3, 4};
	boundaries.ring_vertex_offsets = {0, 4, 7, 10, 13};
	boundaries.ring_vertex_indices = {
	    0, 2, 4, 1, // face0 (A,C,C'-dup,B) — the injected consecutive duplicate
	    0, 1, 3,    // face1 (A,B,D)
	    1, 2, 3,    // face2 (B,C,D)
	    2, 0, 3,    // face3 (C,A,D)
	};

	std::vector<Vertex3D> vertices = {
	    {0, 0, 0}, {2, 0, 0}, {1, 2, 0}, {1, 1, 2}, // A, B, C, D
	    {1, 2, 0},                                  // C' — coordinate-identical to C (index 2)
	};

	auto model = BuildSolidModelFromArrowNative(boundaries, vertices);
	REQUIRE(model.ring_vertex_indices.size() == 12); // 13 raw indices, 1 consecutive duplicate removed
	REQUIRE(model.validation.is_closed);
	REQUIRE(model.validation.is_manifold);
}

TEST_CASE("BuildSolidModelFromArrowNative excludes an unreferenced pool vertex from the model",
          "[arrow_native_import]") {
	// The vertex pool carries a 4th entry (index 3) that no ring ever
	// references. It must not appear in model.vertices at all — in
	// particular it must not pollute ComputeBBox with a point far outside
	// the actual triangle's extent.
	ArrowNativeBoundaries boundaries;
	boundaries.solid_shell_offsets = {0, 1};
	boundaries.shell_face_offsets = {0, 1};
	boundaries.face_ring_offsets = {0, 1};
	boundaries.ring_vertex_offsets = {0, 3};
	boundaries.ring_vertex_indices = {0, 1, 2}; // never references index 3

	std::vector<Vertex3D> vertices = {
	    {0, 0, 0},
	    {1, 0, 0},
	    {0, 1, 0},          // the referenced triangle
	    {1000, 1000, 1000}, // unreferenced — must be excluded
	};

	auto model = BuildSolidModelFromArrowNative(boundaries, vertices);
	REQUIRE(model.vertices.size() == 3);
	REQUIRE(model.bbox.max_x == 1.0);
	REQUIRE(model.bbox.max_y == 1.0);
	REQUIRE(model.bbox.max_z == 0.0);
}

TEST_CASE("BuildSolidModelFromArrowNative flags a degenerate (zero-length) ring, not a crash",
          "[arrow_native_import]") {
	// A face whose only ring has 0 indices (offset delta 0) — passes straight
	// through to the same TriangulateSolidModel/ValidateSolidModel the WKB
	// path already uses, so it must be flagged as degenerate exactly like a
	// degenerate WKB ring is (DESIGN_DOC.md §8.1), not throw or crash here.
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

namespace {

// Cross-encoding parity fixture: the same hollow-cube topology
// test_inner_shell.cpp's WKB-path tests already pin (outer cube [0,4]^3,
// inner cube [1,3]^3, 6 quad faces per shell, inner wound opposite the
// outer), built two ways — once as WKB bytes (this repo's existing ground
// truth: volume 56, surface area 120) and once as already-flattened
// arrow-native boundaries/vertices sharing one 16-vertex pool (indices 0-7
// outer corners, 8-15 inner corners) — to prove the two ingestion paths
// agree on identical input geometry, not just each in isolation.

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

//! Append the six faces of an axis-aligned cube [lo,hi]^3 — identical corner
//! ordering to three_d_extension.cpp's WkbCubeFaces() and
//! test_inner_shell.cpp's AppendCubeFaces().
void AppendCubeFaces(WKBBuilder &b, double lo, double hi, bool reversed) {
	Vertex3D v000 = {lo, lo, lo}, v100 = {hi, lo, lo}, v110 = {hi, hi, lo}, v010 = {lo, hi, lo};
	Vertex3D v001 = {lo, lo, hi}, v101 = {hi, lo, hi}, v111 = {hi, hi, hi}, v011 = {lo, hi, hi};
	auto face = [&](std::vector<Vertex3D> r) {
		if (reversed) {
			std::reverse(r.begin(), r.end());
		}
		b.polyHeader(1);
		b.ring(r);
	};
	face({v000, v010, v110, v100});
	face({v001, v101, v111, v011});
	face({v000, v100, v101, v001});
	face({v010, v011, v111, v110});
	face({v000, v001, v011, v010});
	face({v100, v110, v111, v101});
}

SolidModel BuildHollowCubeFromWKB() {
	WKBBuilder b;
	b.byteOrder();
	b.geomType(WKBGeometryType::PolyhedralSurfaceZ);
	b.u32(12);
	AppendCubeFaces(b, 0.0, 4.0, /*reversed=*/false); // outer shell, faces 0..5
	AppendCubeFaces(b, 1.0, 3.0, /*reversed=*/true);  // inner shell, faces 6..11 (opposite winding)

	auto surfaces = ParseWKB(b.buffer.data(), b.buffer.size());
	GeometryMetadata meta;
	meta.type = "Solid";
	meta.shells = {{6, 6}};
	auto model = BuildSolidModel(surfaces, meta);
	TriangulateSolidModel(model);
	ValidateSolidModel(model);
	return model;
}

SolidModel BuildHollowCubeFromArrowNative() {
	// Shared 16-vertex pool: outer corners at indices 0-7, inner at 8-15 —
	// same corner-to-coordinate assignment AppendCubeFaces()/WkbCubeFaces()
	// use (v000,v100,v110,v010,v001,v101,v111,v011 in that order).
	std::vector<Vertex3D> vertices = {
	    {0, 0, 0}, {4, 0, 0}, {4, 4, 0}, {0, 4, 0}, {0, 0, 4}, {4, 0, 4}, {4, 4, 4}, {0, 4, 4},
	    {1, 1, 1}, {3, 1, 1}, {3, 3, 1}, {1, 3, 1}, {1, 1, 3}, {3, 1, 3}, {3, 3, 3}, {1, 3, 3},
	};

	ArrowNativeBoundaries boundaries;
	boundaries.solid_shell_offsets = {0, 2};    // 1 solid: 2 shells
	boundaries.shell_face_offsets = {0, 6, 12}; // 6 faces per shell
	boundaries.face_ring_offsets.resize(13);
	for (int i = 0; i <= 12; i++) {
		boundaries.face_ring_offsets[i] = static_cast<uint32_t>(i); // 1 ring per face
	}
	boundaries.ring_vertex_offsets.resize(13);
	for (int i = 0; i <= 12; i++) {
		boundaries.ring_vertex_offsets[i] = static_cast<uint32_t>(i * 4); // 4 indices per ring
	}

	// Outer shell (outward winding, matching AppendCubeFaces(reversed=false)):
	// bottom, top, front, back, left, right — corners v000=0,v100=1,v110=2,
	// v010=3,v001=4,v101=5,v111=6,v011=7.
	std::vector<uint32_t> outer = {
	    0, 3, 2, 1, // bottom: v000,v010,v110,v100
	    4, 5, 6, 7, // top: v001,v101,v111,v011
	    0, 1, 5, 4, // front: v000,v100,v101,v001
	    3, 7, 6, 2, // back: v010,v011,v111,v110
	    0, 4, 7, 3, // left: v000,v001,v011,v010
	    1, 2, 6, 5, // right: v100,v110,v111,v101
	};
	// Inner shell: same face order, indices offset by 8, each ring reversed
	// (opposite winding, matching AppendCubeFaces(reversed=true)).
	std::vector<uint32_t> inner = {
	    9,  10, 11, 8,  // bottom reversed
	    15, 14, 13, 12, // top reversed
	    12, 13, 9,  8,  // front reversed
	    10, 14, 15, 11, // back reversed
	    11, 15, 12, 8,  // left reversed
	    13, 14, 10, 9,  // right reversed
	};

	boundaries.ring_vertex_indices.reserve(96);
	boundaries.ring_vertex_indices.insert(boundaries.ring_vertex_indices.end(), outer.begin(), outer.end());
	boundaries.ring_vertex_indices.insert(boundaries.ring_vertex_indices.end(), inner.begin(), inner.end());

	return BuildSolidModelFromArrowNative(boundaries, vertices);
}

} // namespace

TEST_CASE("Arrow-native and WKB ingestion agree on the hollow-cube fixture", "[arrow_native_import][parity]") {
	auto wkb_model = BuildHollowCubeFromWKB();
	auto arrow_model = BuildHollowCubeFromArrowNative();

	REQUIRE(arrow_model.SolidCount() == wkb_model.SolidCount());
	REQUIRE(arrow_model.ShellCount() == wkb_model.ShellCount());
	REQUIRE(arrow_model.FaceCount() == wkb_model.FaceCount());
	REQUIRE(arrow_model.validation.is_closed == wkb_model.validation.is_closed);
	REQUIRE(arrow_model.validation.is_manifold == wkb_model.validation.is_manifold);
	REQUIRE(arrow_model.validation.is_oriented == wkb_model.validation.is_oriented);
	REQUIRE(ComputeVolume(arrow_model) == Approx(ComputeVolume(wkb_model)).epsilon(1e-12));
	REQUIRE(ComputeSurfaceArea(arrow_model) == Approx(ComputeSurfaceArea(wkb_model)).epsilon(1e-12));
	REQUIRE(ComputeVolume(arrow_model) == Approx(56.0).epsilon(1e-12));
	REQUIRE(ComputeSurfaceArea(arrow_model) == Approx(120.0).epsilon(1e-12));
}

namespace {

//! Same hollow-cube topology, but shaped exactly as a real producer
//! (cityparquet-rs's arrow_geom_write.rs, confirmed by reading it) actually
//! emits it: the solid padded to exactly ONE physical shell holding all 12
//! faces flattened together, with the real 2-shell partition recoverable
//! only from geometry_properties.shells — never from the boundaries' own
//! "shell" nesting level.
SolidModel BuildPaddedHollowCubeFromArrowNative() {
	std::vector<Vertex3D> vertices = {
	    {0, 0, 0}, {4, 0, 0}, {4, 4, 0}, {0, 4, 0}, {0, 0, 4}, {4, 0, 4}, {4, 4, 4}, {0, 4, 4},
	    {1, 1, 1}, {3, 1, 1}, {3, 3, 1}, {1, 3, 1}, {1, 1, 3}, {3, 1, 3}, {3, 3, 3}, {1, 3, 3},
	};

	ArrowNativeBoundaries boundaries;
	boundaries.solid_shell_offsets = {0, 1}; // padded: exactly 1 physical shell
	boundaries.shell_face_offsets = {0, 12}; // all 12 faces flattened into it
	boundaries.face_ring_offsets.resize(13);
	for (int i = 0; i <= 12; i++) {
		boundaries.face_ring_offsets[i] = static_cast<uint32_t>(i);
	}
	boundaries.ring_vertex_offsets.resize(13);
	for (int i = 0; i <= 12; i++) {
		boundaries.ring_vertex_offsets[i] = static_cast<uint32_t>(i * 4);
	}

	std::vector<uint32_t> outer = {
	    0, 3, 2, 1, 4, 5, 6, 7, 0, 1, 5, 4, 3, 7, 6, 2, 0, 4, 7, 3, 1, 2, 6, 5,
	};
	std::vector<uint32_t> inner = {
	    9, 10, 11, 8, 15, 14, 13, 12, 12, 13, 9, 8, 10, 14, 15, 11, 11, 15, 12, 8, 13, 14, 10, 9,
	};
	boundaries.ring_vertex_indices.reserve(96);
	boundaries.ring_vertex_indices.insert(boundaries.ring_vertex_indices.end(), outer.begin(), outer.end());
	boundaries.ring_vertex_indices.insert(boundaries.ring_vertex_indices.end(), inner.begin(), inner.end());

	GeometryMetadata metadata;
	metadata.type = "Solid";
	metadata.shells = {{6, 6}}; // the real per-solid shell partition

	return BuildSolidModelFromArrowNative(boundaries, vertices, metadata);
}

} // namespace

TEST_CASE("BuildSolidModelFromArrowNative regroups shells from geometry_properties.shells "
          "when the boundaries are padded to one physical shell",
          "[arrow_native_import]") {
	auto model = BuildPaddedHollowCubeFromArrowNative();
	REQUIRE(model.SolidCount() == 1);
	REQUIRE(model.ShellCount() == 2); // recovered from metadata, not the padded boundaries
	REQUIRE(model.FaceCount() == 12);
	REQUIRE(model.validation.is_closed);
	REQUIRE(model.validation.is_manifold);
	REQUIRE(model.validation.is_oriented); // CheckInteriorShellWinding actually ran
	REQUIRE(ComputeVolume(model) == Approx(56.0).epsilon(1e-12));
	REQUIRE(ComputeSurfaceArea(model) == Approx(120.0).epsilon(1e-12));
}

TEST_CASE("BuildSolidModelFromArrowNative with empty shells metadata keeps the boundaries' own grouping",
          "[arrow_native_import]") {
	ArrowNativeBoundaries boundaries;
	boundaries.solid_shell_offsets = {0, 2};
	boundaries.shell_face_offsets = {0, 1, 2};
	boundaries.face_ring_offsets = {0, 1, 2};
	boundaries.ring_vertex_offsets = {0, 3, 6};
	boundaries.ring_vertex_indices = {0, 1, 2, 3, 4, 5};
	std::vector<Vertex3D> vertices = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}, {0, 0, 1}, {1, 0, 1}, {0, 1, 1}};

	GeometryMetadata metadata; // shells left empty
	auto model = BuildSolidModelFromArrowNative(boundaries, vertices, metadata);
	REQUIRE(model.ShellCount() == 2);
}

TEST_CASE("BuildSolidModelFromArrowNative rejects a shells/boundaries face count mismatch", "[arrow_native_import]") {
	ArrowNativeBoundaries boundaries;
	boundaries.solid_shell_offsets = {0, 1};
	boundaries.shell_face_offsets = {0, 12}; // 12 faces, padded
	boundaries.face_ring_offsets.resize(13);
	for (int i = 0; i <= 12; i++) {
		boundaries.face_ring_offsets[i] = static_cast<uint32_t>(i);
	}
	boundaries.ring_vertex_offsets.resize(13);
	for (int i = 0; i <= 12; i++) {
		boundaries.ring_vertex_offsets[i] = static_cast<uint32_t>(i * 3);
	}
	boundaries.ring_vertex_indices.assign(36, 0);

	GeometryMetadata metadata;
	metadata.type = "Solid";
	metadata.shells = {{6, 5}}; // sums to 11, not 12

	REQUIRE_THROWS_WITH(
	    BuildSolidModelFromArrowNative(boundaries, std::vector<Vertex3D>(1, Vertex3D {0, 0, 0}), metadata),
	    Catch::Contains("face count"));
}
