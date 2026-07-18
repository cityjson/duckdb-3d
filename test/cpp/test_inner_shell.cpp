#include "catch.hpp"
#include "kernel/wkb_parser.hpp"
#include "kernel/model_builder.hpp"
#include "kernel/metadata_parser.hpp"
#include "kernel/validation.hpp"
#include "kernel/measurements.hpp"
#include "kernel/triangulation.hpp"
#include <algorithm>
#include <cstring>

// End-to-end coverage for a SOLID with an interior shell (a hollow solid).
// The signed-volume subtraction documented in DESIGN_DOC §10.2.1 was previously
// unverified: the existing MakeTwoShellWKB test only checked ShellCount. Here we
// build a hollow cube with analytically known volume and surface area and verify
// the full pipeline (build -> validate -> measure), plus two subtler properties:
//
//  * volume is shell-grouping INVARIANT for a disjoint cavity: the plain WKB
//    path (one merged shell) yields the same volume as the metadata path — the
//    subtraction is driven by the interior shell's WINDING, not by grouping.
//    (Grouping only changes ST_3DNumShells.)
//  * validation does NOT yet enforce §9.3's "interior oriented opposite the
//    exterior": a same-wound inner shell passes as valid and yields V_outer +
//    V_inner. That gap is tracked in docs/FUTURE_WORK.md; the test below pins
//    the current behaviour and will flip into a regression test once the
//    cross-shell orientation check is implemented.
//
// Fixture: outer cube [0,4]^3 (V=64, SA=96) enclosing inner cube [1,3]^3
// (V=8, SA=24). Expected hollow volume = 64 - 8 = 56; surface area (all faces,
// shell-agnostic per §10.2.1) = 96 + 24 = 120. All intermediates are small
// integers, so the results are exact in doubles.

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

//! Append the six faces of an axis-aligned cube [lo,hi]^3. reversed=false gives
//! outward-facing winding; reversed=true flips each face (inward-facing), as
//! required for a correctly-oriented interior shell.
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

//! A 12-face PolyhedralSurface: outer cube (outward) + inner cube. When
//! inner_inward=true the interior shell is wound inward (correct hollow solid);
//! when false, both shells face outward (the mis-oriented interior case).
std::vector<uint8_t> MakeHollowCubeWKB(bool inner_inward) {
	WKBBuilder b;
	b.byteOrder();
	b.geomType(WKBGeometryType::PolyhedralSurfaceZ);
	b.u32(12);
	AppendCubeFaces(b, 0.0, 4.0, /*reversed=*/false);        // outer shell, faces 0..5
	AppendCubeFaces(b, 1.0, 3.0, /*reversed=*/inner_inward); // inner shell, faces 6..11
	return b.buffer;
}

SolidModel BuildHollowCube(bool with_metadata, bool inner_inward = true) {
	auto wkb = MakeHollowCubeWKB(inner_inward);
	auto surfaces = ParseWKB(wkb.data(), wkb.size());
	SolidModel model;
	if (with_metadata) {
		GeometryMetadata meta;
		meta.type = "Solid";
		meta.shells = {{6, 6}};
		model = BuildSolidModel(surfaces, meta);
	} else {
		model = BuildSolidModel(surfaces);
	}
	TriangulateSolidModel(model);
	ValidateSolidModel(model);
	return model;
}

} // namespace

TEST_CASE("Hollow solid: metadata groups faces into one solid, two shells", "[inner_shell]") {
	auto model = BuildHollowCube(/*with_metadata=*/true);
	REQUIRE(model.SolidCount() == 1);
	REQUIRE(model.ShellCount() == 2);
	REQUIRE(model.FaceCount() == 12);
}

TEST_CASE("Hollow solid: validates as closed, manifold, oriented", "[inner_shell]") {
	auto model = BuildHollowCube(/*with_metadata=*/true);
	REQUIRE(model.validation.is_closed);
	REQUIRE(model.validation.is_manifold);
	REQUIRE(model.validation.is_oriented);
	REQUIRE(model.validation.degenerate_face_count == 0);
	REQUIRE(model.validation.is_valid);
}

TEST_CASE("Hollow solid: volume subtracts the interior shell (64 - 8 = 56)", "[inner_shell]") {
	auto model = BuildHollowCube(/*with_metadata=*/true);
	REQUIRE(ComputeVolume(model) == Approx(56.0).epsilon(1e-12));
}

TEST_CASE("Hollow solid: surface area sums all faces incl. cavity walls (96 + 24 = 120)", "[inner_shell]") {
	auto model = BuildHollowCube(/*with_metadata=*/true);
	REQUIRE(ComputeSurfaceArea(model) == Approx(120.0).epsilon(1e-12));
}

TEST_CASE("Hollow solid: volume is shell-grouping invariant for a disjoint cavity", "[inner_shell]") {
	// Same bytes, plain import (no metadata) -> one merged shell. The two cubes
	// share no edges, so per-edge cancellation still holds: the merged shell is
	// closed/manifold and the signed sum is identical. Volume matches the grouped
	// case; only ShellCount differs. (Corrects the old "rejected as non-manifold"
	// assumption for disjoint cavities.)
	auto grouped = BuildHollowCube(/*with_metadata=*/true);
	auto plain = BuildHollowCube(/*with_metadata=*/false);
	REQUIRE(plain.ShellCount() == 1);
	REQUIRE(grouped.ShellCount() == 2);
	REQUIRE(plain.validation.is_closed);
	REQUIRE(ComputeVolume(plain) == Approx(ComputeVolume(grouped)).epsilon(1e-12));
	REQUIRE(ComputeVolume(plain) == Approx(56.0).epsilon(1e-12));
}

TEST_CASE("Hollow solid: a same-wound interior shell is NOT subtracted (documents §9.3 gap)", "[inner_shell]") {
	// The interior shell wound OUTWARD (same as exterior) instead of inward.
	// DESIGN_DOC §9.3 promises interior shells are oriented opposite the exterior,
	// but ValidateSolidModel checks orientation per-shell only and does not enforce
	// the cross-shell relationship (tracked in docs/FUTURE_WORK.md). So this still
	// validates and the volume ADDS (64 + 8 = 72) rather than subtracting. When the
	// cross-shell check lands, this becomes the regression test that drives it.
	auto model = BuildHollowCube(/*with_metadata=*/true, /*inner_inward=*/false);
	REQUIRE(model.validation.is_valid); // current (imperfect) behaviour
	REQUIRE(ComputeVolume(model) == Approx(72.0).epsilon(1e-12));
}
