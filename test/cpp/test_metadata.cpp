#include "catch.hpp"
#include "kernel/metadata_parser.hpp"
#include "kernel/model_builder.hpp"
#include "kernel/wkb_parser.hpp"
#include <cstring>

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
	//! Per ISO WKB, each Polygon nested under a PolyhedralSurface is its own
	//! WKB value with byte-order + type code + num_rings header.
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

//! Build a PolyhedralSurface Z with 8 triangular faces (simulating a
//! Solid with 2 shells: outer tetra (4 faces) + inner tetra (4 faces))
std::vector<uint8_t> MakeTwoShellWKB() {
	Vertex3D v0 = {0, 0, 0}, v1 = {2, 0, 0}, v2 = {1, 2, 0}, v3 = {1, 1, 2};
	Vertex3D i0 = {0.5, 0.5, 0.3}, i1 = {1.5, 0.5, 0.3}, i2 = {1, 1.5, 0.3}, i3 = {1, 1, 1.2};

	WKBBuilder b;
	b.byteOrder();
	b.geomType(WKBGeometryType::PolyhedralSurfaceZ);
	b.u32(8); // 8 faces total

	// Outer shell: 4 faces
	b.polyHeader(1);
	b.ring({v0, v2, v1});
	b.polyHeader(1);
	b.ring({v0, v1, v3});
	b.polyHeader(1);
	b.ring({v1, v2, v3});
	b.polyHeader(1);
	b.ring({v2, v0, v3});

	// Inner shell: 4 faces (reversed winding for interior)
	b.polyHeader(1);
	b.ring({i0, i1, i2});
	b.polyHeader(1);
	b.ring({i0, i3, i1});
	b.polyHeader(1);
	b.ring({i1, i3, i2});
	b.polyHeader(1);
	b.ring({i2, i3, i0});

	return b.buffer;
}

} // anonymous namespace

// Spec §8 geometry_properties: `type` is a CityJSON string and `shells` carries
// per-shell emitted-face counts — a flat array for a Solid, one array per solid
// (nested) for MultiSolid/CompositeSolid.

TEST_CASE("ParseGeometryProperties: Solid with flat shells", "[metadata]") {
	auto meta = ParseGeometryProperties(R"({"type": "Solid", "shells": [12]})");
	REQUIRE(meta.type == "Solid");
	REQUIRE(meta.shells.size() == 1);    // one solid
	REQUIRE(meta.shells[0].size() == 1); // one shell
	REQUIRE(meta.shells[0][0] == 12);
}

TEST_CASE("ParseGeometryProperties: Solid with two shells (flat)", "[metadata]") {
	auto meta = ParseGeometryProperties(R"({"type": "Solid", "shells": [4, 4]})");
	REQUIRE(meta.type == "Solid");
	REQUIRE(meta.shells.size() == 1);
	REQUIRE(meta.shells[0] == std::vector<uint32_t>({4, 4}));
}

TEST_CASE("ParseGeometryProperties: Solid with singly-nested shells (cityparquet-rs STRUCT shape)", "[metadata]") {
	// cityparquet-rs's geometry_properties_lod* STRUCT always nests `shells` as
	// List<List<Int32>> — even a single Solid serializes (e.g. via to_json() on
	// the Arrow STRUCT) as [[4, 4]], not the flat [4, 4] duckdb-cityjson emits.
	// Both forms must parse to the identical GeometryMetadata.
	auto meta = ParseGeometryProperties(R"({"type": "Solid", "shells": [[4, 4]]})");
	REQUIRE(meta.type == "Solid");
	REQUIRE(meta.shells.size() == 1); // one solid
	REQUIRE(meta.shells[0] == std::vector<uint32_t>({4, 4}));
}

TEST_CASE("ParseGeometryProperties: CompositeSolid with nested shells", "[metadata]") {
	auto meta = ParseGeometryProperties(R"({"type": "CompositeSolid", "shells": [[12], [8, 4]]})");
	REQUIRE(meta.type == "CompositeSolid");
	REQUIRE(meta.shells.size() == 2); // two solids
	REQUIRE(meta.shells[0] == std::vector<uint32_t>({12}));
	REQUIRE(meta.shells[1] == std::vector<uint32_t>({8, 4}));
}

TEST_CASE("ParseGeometryProperties: string type without shells (non-solid)", "[metadata]") {
	auto meta = ParseGeometryProperties(R"({"type": "MultiSurface", "surfaces": [], "face_semantics": []})");
	REQUIRE(meta.type == "MultiSurface");
	REQUIRE(meta.shells.empty());
}

TEST_CASE("ParseGeometryProperties: empty JSON", "[metadata]") {
	auto meta = ParseGeometryProperties("");
	REQUIRE(meta.type.empty());
	REQUIRE(meta.shells.empty());
}

TEST_CASE("ParseGeometryProperties: malformed JSON raises", "[metadata]") {
	REQUIRE_THROWS_WITH(ParseGeometryProperties("{"), Catch::Contains("JSON"));
}

TEST_CASE("ParseGeometryProperties: a shell count above uint32 range raises", "[metadata]") {
	// 2^32 exceeds a uint32 face count; reject rather than truncate-and-wrap.
	REQUIRE_THROWS_WITH(ParseGeometryProperties(R"({"type": "Solid", "shells": [4294967296]})"),
	                    Catch::Contains("out of range"));
}

TEST_CASE("BuildSolidModel with metadata: a zero shell count is a dropped shell", "[metadata]") {
	// A `0` in `shells` (spec §8: a fully-dropped shell) creates no shell. Here
	// [0, 8] on an 8-face surface yields a single 8-face shell.
	auto wkb = MakeTwoShellWKB();
	auto surfaces = ParseWKB(wkb.data(), wkb.size());
	GeometryMetadata meta;
	meta.type = "Solid";
	meta.shells = {{0, 8}};
	auto model = BuildSolidModel(surfaces, meta);
	REQUIRE(model.SolidCount() == 1);
	REQUIRE(model.ShellCount() == 1);
	REQUIRE(model.FaceCount() == 8);
}

TEST_CASE("BuildSolidModel with metadata: split into 2 shells", "[metadata]") {
	auto wkb = MakeTwoShellWKB();
	auto surfaces = ParseWKB(wkb.data(), wkb.size());
	REQUIRE(surfaces.size() == 1);
	REQUIRE(surfaces[0].polygon_count == 8);

	GeometryMetadata meta;
	meta.type = "Solid";
	meta.shells = {{4, 4}};

	auto model = BuildSolidModel(surfaces, meta);
	REQUIRE(model.SolidCount() == 1);
	REQUIRE(model.ShellCount() == 2);
	REQUIRE(model.FaceCount() == 8);
}

TEST_CASE("BuildSolidModel with metadata: plain import (no shell split)", "[metadata]") {
	auto wkb = MakeTwoShellWKB();
	auto surfaces = ParseWKB(wkb.data(), wkb.size());

	// Without metadata: single shell
	auto model = BuildSolidModel(surfaces);
	REQUIRE(model.SolidCount() == 1);
	REQUIRE(model.ShellCount() == 1);
	REQUIRE(model.FaceCount() == 8);
}

TEST_CASE("BuildSolidModel with metadata: conflict raises", "[metadata]") {
	auto wkb = MakeTwoShellWKB();
	auto surfaces = ParseWKB(wkb.data(), wkb.size());

	GeometryMetadata meta;
	meta.type = "Solid";
	meta.shells = {{3, 3}}; // sum=6 but WKB has 8 faces → conflict

	REQUIRE_THROWS_WITH(BuildSolidModel(surfaces, meta), Catch::Contains("face count mismatch"));
}

TEST_CASE("BuildSolidModel with metadata: multi-solid with nested shells (G2)", "[metadata]") {
	// A GeometryCollectionZ of two PolyhedralSurfaces (8 faces each), each a
	// two-shell hollow solid. Nested `shells` [[4,4],[4,4]] builds two solids,
	// two shells apiece — previously unsupported (threw).
	auto shell_wkb = MakeTwoShellWKB();
	WKBBuilder b;
	b.byteOrder();
	b.geomType(WKBGeometryType::GeometryCollectionZ);
	b.u32(2);
	b.buffer.insert(b.buffer.end(), shell_wkb.begin(), shell_wkb.end());
	b.buffer.insert(b.buffer.end(), shell_wkb.begin(), shell_wkb.end());

	auto surfaces = ParseWKB(b.buffer.data(), b.buffer.size());
	REQUIRE(surfaces.size() == 2);

	GeometryMetadata meta;
	meta.type = "MultiSolid";
	meta.shells = {{4, 4}, {4, 4}};

	auto model = BuildSolidModel(surfaces, meta);
	REQUIRE(model.SolidCount() == 2);
	REQUIRE(model.ShellCount() == 4);
	REQUIRE(model.FaceCount() == 16);
}

TEST_CASE("BuildSolidModel with metadata: shells member count mismatch raises", "[metadata]") {
	// Two WKB members but only one shells array → cannot map shells to solids.
	auto shell_wkb = MakeTwoShellWKB();
	WKBBuilder b;
	b.byteOrder();
	b.geomType(WKBGeometryType::GeometryCollectionZ);
	b.u32(2);
	b.buffer.insert(b.buffer.end(), shell_wkb.begin(), shell_wkb.end());
	b.buffer.insert(b.buffer.end(), shell_wkb.begin(), shell_wkb.end());

	auto surfaces = ParseWKB(b.buffer.data(), b.buffer.size());

	GeometryMetadata meta;
	meta.type = "MultiSolid";
	meta.shells = {{4, 4}}; // only one solid's shells for two WKB members

	REQUIRE_THROWS_WITH(BuildSolidModel(surfaces, meta), Catch::Contains("solid count"));
}
