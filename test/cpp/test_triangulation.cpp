#include "catch.hpp"
#include "kernel/triangulation.hpp"
#include "kernel/measurements.hpp"
#include "kernel/solid_model.hpp"
#include "kernel/validation.hpp"
#include "real_rings.hpp"
#include <array>
#include <cstdint>
#include "mapbox/earcut.hpp"
#include <cmath>
#include <map>
#include <tuple>

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

//! A SolidModel holding one face made of `rings` (ring 0 exterior, the rest
//! holes), with vertices deduplicated exactly as the model builder does, so a
//! pinched ring references its repeated vertex by one index.
SolidModel FaceWithRings(const real_rings::Rings &rings) {
	SolidModel m;
	std::map<std::tuple<double, double, double>, uint32_t> index;
	m.solid_shell_offsets = {0, 1};
	m.shell_face_offsets = {0, 1};
	m.face_ring_offsets = {0, static_cast<uint32_t>(rings.size())};
	m.ring_vertex_offsets = {0};
	for (const auto &ring : rings) {
		for (const auto &v : ring) {
			auto key = std::make_tuple(v.x, v.y, v.z);
			auto it = index.find(key);
			if (it == index.end()) {
				it = index.emplace(key, static_cast<uint32_t>(m.vertices.size())).first;
				m.vertices.push_back(v);
			}
			m.ring_vertex_indices.push_back(it->second);
		}
		m.ring_vertex_offsets.push_back(static_cast<uint32_t>(m.ring_vertex_indices.size()));
	}
	TriangulateSolidModel(m);
	return m;
}

//! Twice the vector area of triangle t, taken about vertex `o` for conditioning.
Vertex3D TriangleAreaVector2(const SolidModel &m, uint32_t t, const Vertex3D &o) {
	const auto &a = m.vertices[m.triangle_vertex_indices[t * 3 + 0]];
	const auto &b = m.vertices[m.triangle_vertex_indices[t * 3 + 1]];
	const auto &c = m.vertices[m.triangle_vertex_indices[t * 3 + 2]];
	double ax = a.x - o.x, ay = a.y - o.y, az = a.z - o.z;
	double bx = b.x - o.x - ax, by = b.y - o.y - ay, bz = b.z - o.z - az;
	double cx = c.x - o.x - ax, cy = c.y - o.y - ay, cz = c.z - o.z - az;
	return {by * cz - bz * cy, bz * cx - bx * cz, bx * cy - by * cx};
}

//! The triangulation tiles the face. Two checks, both against the face's
//! independently computed area:
//!   * the triangles' summed vector area equals it — the vector area of a closed
//!     polygon is the same for every triangulation that uses all of its vertices
//!     with consistent winding, planar or not, so a triangulation that stops
//!     early or skips a vertex falls short;
//!   * their unsigned areas along the face normal add up to it, up to
//!     `fold_slack` — a hole triangulated on its own lays triangles over the
//!     exterior's, which the vector sum alone cannot see. On a planar face the
//!     slack is rounding; a non-planar face has genuine folds, so the caller
//!     states how much.
void RequireTilesFace(const SolidModel &m, double expected_area, double fold_slack = 1e-9) {
	REQUIRE(m.TriangleCount() > 0);
	const Vertex3D o = m.vertices[0];
	Vertex3D sum = {0, 0, 0};
	for (uint32_t t = 0; t < m.TriangleCount(); t++) {
		auto v = TriangleAreaVector2(m, t, o);
		sum.x += v.x;
		sum.y += v.y;
		sum.z += v.z;
	}
	double len = std::sqrt(sum.x * sum.x + sum.y * sum.y + sum.z * sum.z);
	REQUIRE(0.5 * len == Approx(expected_area).epsilon(1e-9));
	Vertex3D n = {sum.x / len, sum.y / len, sum.z / len};
	double unsigned_along = 0;
	for (uint32_t t = 0; t < m.TriangleCount(); t++) {
		auto v = TriangleAreaVector2(m, t, o);
		unsigned_along += std::abs(0.5 * (v.x * n.x + v.y * n.y + v.z * n.z));
	}
	REQUIRE(unsigned_along <= expected_area * (1 + fold_slack));
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
			auto m = OneFace(
			    {{D, D, 0}, {D + 3, D, 0}, {D + 3, D + 1, 0}, {D + 1, D + 1, 0}, {D + 1, D + 3, 0}, {D, D + 3, 0}});
			INFO("offset = " << D << ", triangles = " << m.TriangleCount());
			REQUIRE(m.TriangleCount() == 4);
			REQUIRE(ComputeSurfaceArea(m) == Approx(5.0).epsilon(1e-9));
		}
	}
}

TEST_CASE("Triangulation: a hole is bridged into its face, not triangulated on its own", "[triangulation]") {
	// 4 x 4 square with a 2 x 2 hole, the hole wound opposite the exterior as a
	// polygon's interior ring is. The face's area is 16 - 4 = 12; triangulating the
	// hole as a separate polygon would lay 4 extra units over the exterior's own
	// triangles, so the triangles would not tile the face.
	auto m =
	    FaceWithRings({{{0, 0, 0}, {4, 0, 0}, {4, 4, 0}, {0, 4, 0}}, {{1, 1, 0}, {1, 3, 0}, {3, 3, 0}, {3, 1, 0}}});
	RequireTilesFace(m, 12.0);
	REQUIRE(m.TriangleCount() == 8); // n + 2h - 2 = 8 + 2 - 2
}

TEST_CASE("Triangulation: real rings pinched at a repeated vertex triangulate completely", "[triangulation]") {
	// 3DBAG roof faces whose exterior ring touches itself at one vertex; the
	// repeated vertex is one model vertex, so the ring visits it twice.
	SECTION("NL.IMBAG.Pand.0503100000029374-0 face 39") {
		RequireTilesFace(FaceWithRings(real_rings::kDelft29374Face39), real_rings::kDelft29374Face39Area);
	}
	SECTION("NL.IMBAG.Pand.0503100000019817-0 face 82") {
		RequireTilesFace(FaceWithRings(real_rings::kDelft19817Face82), real_rings::kDelft19817Face82Area);
	}
	SECTION("NL.IMBAG.Pand.0503100000000010-0 face 928, with a hole") {
		RequireTilesFace(FaceWithRings(real_rings::kDelft00010Face928), real_rings::kDelft00010Face928Area);
	}
}

TEST_CASE("Triangulation: a real face with four holes tiles its area", "[triangulation]") {
	// lod3_railway.city.json: a 30-vertex exterior ring with four interior rings.
	RequireTilesFace(FaceWithRings(real_rings::kRailway5c9d78e9Face12), real_rings::kRailway5c9d78e9Face12Area);
}

TEST_CASE("Triangulation: real faces where ear clipping stopped early triangulate completely", "[triangulation]") {
	SECTION("a twisted quad, GMLID_46150217_194492_1237 face 771") {
		RequireTilesFace(FaceWithRings(real_rings::kRailway46150217Face771), real_rings::kRailway46150217Face771Area);
	}
	SECTION("a face with collinear runs, GMLID_6162422_289094_1279 face 168") {
		// Not planar: five vertices lie in x = 10.34, two do not, so the part in
		// x = 10.34 folds against the face's mean plane by a sliver of about
		// 1.7e-6 against an area of 7.2e-4.
		RequireTilesFace(FaceWithRings(real_rings::kRailway6162422Face168), real_rings::kRailway6162422Face168Area,
		                 1e-2);
	}
}

TEST_CASE("Triangulation: a face it cannot tile gets no triangles and is degenerate", "[triangulation]") {
	// A self-intersecting ring: edges (0,0)-(4,4) and (4,0)-(0,2) cross at
	// (4/3, 4/3). Its signed area is 4, its two lobes cover 16/3 + 4/3 = 20/3, so
	// no set of triangles both tiles it and matches its area. The triangulation
	// must not hand back a partial or overlapping result: the face gets no
	// triangles, and validation counts it degenerate, which gates ST_3DVolume and
	// ST_3DSurfaceArea.
	auto m = FaceWithRings({{{0, 0, 0}, {4, 4, 0}, {4, 0, 0}, {0, 2, 0}}});
	REQUIRE(m.TriangleCount() == 0);
	ValidateSolidModel(m);
	REQUIRE(m.validation.degenerate_face_count == 1);
	REQUIRE_FALSE(m.validation.is_valid);
}

namespace {

//! A self-overlapping ring: an 8000-step lattice random walk from a fixed-seed
//! linear congruential generator, consecutive repeats dropped. Earcut finds no
//! ear on such a ring and falls back to splitting it, and the splits nest:
//! unbounded, this ring nests 51 deep (measured on earcut 2.2.4), past the
//! 32-level bound, while a fixture face nests at most once. The ring is the size
//! it is because the nesting grows with it — about 15 at 1000 steps, 35 at 4000
//! — and 8000 steps clear the bound with margin while triangulating in well
//! under a second.
std::vector<Vertex3D> SelfOverlappingWalk() {
	std::vector<Vertex3D> ring;
	uint32_t state = 12345;
	double x = 0, y = 0;
	for (int i = 0; i < 8000; i++) {
		state = state * 1664525u + 1013904223u;
		x += static_cast<double>((state >> 16) % 3) - 1;
		state = state * 1664525u + 1013904223u;
		y += static_cast<double>((state >> 16) % 3) - 1;
		if (!ring.empty() && ring.back().x == x && ring.back().y == y) {
			continue;
		}
		ring.push_back({x, y, 0});
	}
	while (ring.size() > 1 && ring.front().x == ring.back().x && ring.front().y == ring.back().y) {
		ring.pop_back();
	}
	return ring;
}

} // namespace

TEST_CASE("Triangulation: earcut's split recursion is bounded", "[triangulation]") {
	// Each split recurses on both halves, so on a self-overlapping ring the nesting
	// tracks the ring's size; ring input arrives from SQL, so unbounded nesting
	// could exhaust a worker thread's stack. The vendored earcut caps it.
	auto ring = SelfOverlappingWalk();
	std::vector<std::vector<std::array<double, 2>>> polygon(1);
	for (const auto &v : ring) {
		polygon[0].push_back({v.x, v.y});
	}
	mapbox::detail::Earcut<uint32_t> earcut;
	earcut(polygon);
	REQUIRE(earcut.splitDepthExceeded);
}

TEST_CASE("Triangulation: a face that hits earcut's split bound gets no triangles and is degenerate",
          "[triangulation]") {
	auto m = FaceWithRings({SelfOverlappingWalk()});
	REQUIRE(m.TriangleCount() == 0);
	ValidateSolidModel(m);
	REQUIRE(m.validation.degenerate_face_count == 1);
}
