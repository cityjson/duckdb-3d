#include "catch.hpp"
#include "kernel/triangulation.hpp"
#include "kernel/measurements.hpp"
#include "kernel/solid_model.hpp"
#include <cmath>

// Direct unit coverage for kernel/triangulation (ear-clipping). Previously the
// triangulator was only exercised transitively through whole-solid measurement
// tests. Here we triangulate single faces with known area and triangle counts,
// including a concave (L-shaped) face — the case naive fan triangulation gets
// wrong but ear-clipping must handle.

using namespace duckdb_3d;

namespace {

//! A SolidModel holding exactly one face (one solid, one shell, one ring).
//! Not closed — triangulation and per-face area do not require closedness.
SolidModel OneFace(const std::vector<Vertex3D> &ring) {
	SolidModel m;
	m.vertices = ring;
	m.solid_shell_offsets = {0, 1};
	m.shell_face_offsets = {0, 1};
	m.face_ring_offsets = {0, 1};
	m.ring_vertex_offsets = {0, static_cast<uint32_t>(ring.size())};
	m.ring_vertex_indices.resize(ring.size());
	for (uint32_t i = 0; i < ring.size(); i++) {
		m.ring_vertex_indices[i] = i;
	}
	TriangulateSolidModel(m);
	return m;
}

} // namespace

TEST_CASE("Triangulation: a convex quad splits into 2 triangles", "[triangulation]") {
	auto m = OneFace({{0, 0, 0}, {2, 0, 0}, {2, 2, 0}, {0, 2, 0}});
	REQUIRE(m.TriangleCount() == 2);
	// The two triangles tile the quad exactly: total area = 2 * 2 = 4.
	REQUIRE(ComputeSurfaceArea(m) == Approx(4.0).epsilon(1e-12));
}

TEST_CASE("Triangulation: an n-gon yields n-2 triangles", "[triangulation]") {
	// Regular-ish pentagon in the z=0 plane.
	auto m = OneFace({{0, 0, 0}, {2, 0, 0}, {3, 2, 0}, {1, 3, 0}, {-1, 2, 0}});
	REQUIRE(m.TriangleCount() == 3); // 5 - 2
}

TEST_CASE("Triangulation: a concave L-shaped face triangulates to the correct area", "[triangulation]") {
	// L-shape: 3x3 square minus a 2x2 corner -> area 5. Concave at (1,1); a naive
	// fan from vertex 0 would emit a triangle outside the polygon, so this pins
	// that ear-clipping respects concavity.
	auto m = OneFace({{0, 0, 0}, {3, 0, 0}, {3, 1, 0}, {1, 1, 0}, {1, 3, 0}, {0, 3, 0}});
	REQUIRE(m.TriangleCount() == 4); // 6 - 2
	REQUIRE(ComputeSurfaceArea(m) == Approx(5.0).epsilon(1e-12));
}

TEST_CASE("Triangulation: a tilted (non-axis-aligned) face keeps its true area", "[triangulation]") {
	// Unit square lying in the plane z = x (tilted 45°); true area = sqrt(2).
	auto m = OneFace({{0, 0, 0}, {1, 0, 1}, {1, 1, 1}, {0, 1, 0}});
	REQUIRE(m.TriangleCount() == 2);
	REQUIRE(ComputeSurfaceArea(m) == Approx(std::sqrt(2.0)).epsilon(1e-12));
}

TEST_CASE("Triangulation: ring winding is decided about a local origin", "[triangulation]") {
	// EarClipTriangulate decides the ring's 2D handedness with a shoelace sum.
	// Written on absolute projected coordinates, px[i]*py[j] - px[j]*py[i], its
	// intermediate products scale as |position|^2 while the answer scales as
	// |extent|^2 — the same conditioning trap as the volume sum (DESIGN_DOC
	// §8.2), one power lower. When the noise swamps the true value the handedness
	// flips, the convexity test inverts, no ear is ever found and the face silently
	// emits ZERO triangles: ST_3DVolume then returns a wrong number without any
	// validity flag changing, because validation works on rings, not triangles.
	//
	// The threshold scales with the FACE's own area, not the model's extent: the
	// shoelace noise is ~n·|p|²·eps, so a face breaks once its area falls below
	// that. A 2 m square survives to ~1e9; a 1 mm face already breaks at RD New
	// (EPSG:28992) northings, where the sum collapses to exactly 0 and the ring
	// is read as clockwise.
	SECTION("2 m square, far from the origin") {
		for (double D : {0.0, 4.5e5, 1.0e7, 1.0e9}) {
			auto m = OneFace({{D, D, 0}, {D + 2, D, 0}, {D + 2, D + 2, 0}, {D, D + 2, 0}});
			INFO("offset = " << D << ", triangles = " << m.TriangleCount());
			REQUIRE(m.TriangleCount() == 2);
			REQUIRE(ComputeSurfaceArea(m) == Approx(4.0).epsilon(1e-9));
		}
	}
	SECTION("1 mm face at RD New magnitudes") {
		const double s = 0.001;
		for (double D : {0.0, 8.5e4, 4.5e5}) {
			auto m = OneFace({{D, D, 0}, {D + s, D, 0}, {D + s, D + s, 0}, {D, D + s, 0}});
			INFO("offset = " << D << ", triangles = " << m.TriangleCount());
			REQUIRE(m.TriangleCount() == 2);
			REQUIRE(ComputeSurfaceArea(m) == Approx(s * s).epsilon(1e-6));
		}
	}
	SECTION("concave L-shape far from the origin") {
		for (double D : {0.0, 4.5e5, 1.0e9}) {
			auto m = OneFace({{D, D, 0},
			                  {D + 3, D, 0},
			                  {D + 3, D + 1, 0},
			                  {D + 1, D + 1, 0},
			                  {D + 1, D + 3, 0},
			                  {D, D + 3, 0}});
			INFO("offset = " << D << ", triangles = " << m.TriangleCount());
			REQUIRE(m.TriangleCount() == 4);
			REQUIRE(ComputeSurfaceArea(m) == Approx(5.0).epsilon(1e-9));
		}
	}
}
