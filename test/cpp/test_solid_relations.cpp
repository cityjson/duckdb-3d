#include "catch.hpp"
#include "kernel/geom_distance.hpp"
#include "kernel/metadata_parser.hpp"
#include "kernel/model_builder.hpp"
#include "kernel/solid_relations.hpp"
#include "kernel/triangulation.hpp"
#include "kernel/validation.hpp"
#include "kernel/wkb_parser.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <random>

using namespace duckdb_3d;

namespace {

Vertex3D V(double x, double y, double z) {
	return Vertex3D {x, y, z};
}

double Up(double v) {
	return std::nextafter(v, std::numeric_limits<double>::infinity());
}

class WKBBuilder {
public:
	std::vector<uint8_t> buffer;
	void u8(uint8_t v) {
		buffer.push_back(v);
	}
	void u32(uint32_t v) {
		for (int i = 0; i < 4; i++) {
			buffer.push_back((v >> (8 * i)) & 0xFF);
		}
	}
	void f64(double v) {
		uint8_t b[8];
		std::memcpy(b, &v, 8);
		buffer.insert(buffer.end(), b, b + 8);
	}
	void face(std::vector<Vertex3D> ring) {
		u8(1);
		u32(static_cast<uint32_t>(WKBGeometryType::PolygonZ));
		u32(1);
		ring.push_back(ring.front());
		u32(static_cast<uint32_t>(ring.size()));
		for (const auto &p : ring) {
			f64(p.x);
			f64(p.y);
			f64(p.z);
		}
	}
};

using Faces = std::vector<std::vector<Vertex3D>>;

//! The six faces of the box [lo, hi], wound outward (or inward when reversed).
Faces BoxFaces(const Vertex3D &lo, const Vertex3D &hi, bool reversed = false) {
	Vertex3D v000 = lo, v100 = V(hi.x, lo.y, lo.z), v110 = V(hi.x, hi.y, lo.z), v010 = V(lo.x, hi.y, lo.z);
	Vertex3D v001 = V(lo.x, lo.y, hi.z), v101 = V(hi.x, lo.y, hi.z), v111 = hi, v011 = V(lo.x, hi.y, hi.z);
	Faces faces = {{v000, v010, v110, v100}, {v001, v101, v111, v011}, {v000, v100, v101, v001},
	               {v010, v011, v111, v110}, {v000, v001, v011, v010}, {v100, v110, v111, v101}};
	if (reversed) {
		for (auto &f : faces) {
			std::reverse(f.begin(), f.end());
		}
	}
	return faces;
}

//! A solid from faces; `shells` groups them (one shell when empty).
SolidModel MakeSolid(const Faces &faces, const ShellCounts &shells = {}) {
	WKBBuilder b;
	b.u8(1);
	b.u32(static_cast<uint32_t>(WKBGeometryType::PolyhedralSurfaceZ));
	b.u32(static_cast<uint32_t>(faces.size()));
	for (const auto &f : faces) {
		b.face(f);
	}
	auto surfaces = ParseWKB(b.buffer.data(), b.buffer.size());
	SolidModel model;
	if (shells.empty()) {
		model = BuildSolidModel(surfaces);
	} else {
		GeometryMetadata meta;
		meta.type = "Solid";
		meta.shells = shells;
		model = BuildSolidModel(surfaces, meta);
	}
	TriangulateSolidModel(model);
	ValidateSolidModel(model);
	return model;
}

SolidModel Box(const Vertex3D &lo, const Vertex3D &hi) {
	return MakeSolid(BoxFaces(lo, hi));
}

//! The box [0, 4]^3 with the cavity [1, 3]^3.
SolidModel HollowBox() {
	auto faces = BoxFaces(V(0, 0, 0), V(4, 4, 4));
	auto inner = BoxFaces(V(1, 1, 1), V(3, 3, 3), /*reversed=*/true);
	faces.insert(faces.end(), inner.begin(), inner.end());
	return MakeSolid(faces, {{6, 6}});
}

//! An L-shaped prism, height 2: the square [0, 2]^2 without its (1..2, 1..2)
//! quadrant.
SolidModel LPrism() {
	std::vector<std::array<double, 2>> ring = {{0, 0}, {2, 0}, {2, 1}, {1, 1}, {1, 2}, {0, 2}};
	Faces faces;
	std::vector<Vertex3D> bottom, top;
	for (auto it = ring.rbegin(); it != ring.rend(); ++it) {
		bottom.push_back(V((*it)[0], (*it)[1], 0));
	}
	for (const auto &p : ring) {
		top.push_back(V(p[0], p[1], 2));
	}
	faces.push_back(bottom);
	faces.push_back(top);
	for (size_t i = 0; i < ring.size(); i++) {
		const auto &p = ring[i];
		const auto &q = ring[(i + 1) % ring.size()];
		faces.push_back({V(p[0], p[1], 0), V(q[0], q[1], 0), V(q[0], q[1], 2), V(p[0], p[1], 2)});
	}
	return MakeSolid(faces);
}

GeomModel Point(double x, double y, double z) {
	GeomModel m;
	m.type = GeomType::Point;
	m.vertices = {V(x, y, z)};
	m.ComputeBBox();
	return m;
}

GeomModel Segment(const Vertex3D &a, const Vertex3D &b) {
	GeomModel m;
	m.type = GeomType::LineString;
	m.vertices = {a, b};
	m.ComputeBBox();
	return m;
}

GeomModel Square(double x0, double y0, double z, double side) {
	GeomModel m;
	m.type = GeomType::Polygon;
	m.vertices = {V(x0, y0, z), V(x0 + side, y0, z), V(x0 + side, y0 + side, z), V(x0, y0 + side, z)};
	m.ring_offsets = {0, 4};
	m.ComputeBBox();
	return m;
}

} // namespace

TEST_CASE("PointInSolidVolume: a box, including rays along its triangulation's diagonals", "[solid_relations]") {
	auto box = Box(V(0, 0, 0), V(4, 4, 4));
	REQUIRE(box.validation.is_valid);
	// (2, 2) lies on the diagonal of the top and bottom faces' triangulations, so
	// a vertical ray from these points meets two triangles' shared edge.
	REQUIRE(PointInSolidVolume(box, V(2, 2, 2)));
	REQUIRE(PointInSolidVolume(box, V(1, 1, 3)));
	REQUIRE(PointInSolidVolume(box, V(3, 1, 0.5)));
	REQUIRE_FALSE(PointInSolidVolume(box, V(2, 2, -1)));
	REQUIRE_FALSE(PointInSolidVolume(box, V(2, 2, 5)));
	REQUIRE_FALSE(PointInSolidVolume(box, V(5, 2, 2)));
	// Projections onto the box's vertical walls and corners.
	REQUIRE_FALSE(PointInSolidVolume(box, V(4, 4, 6)));
	REQUIRE_FALSE(PointInSolidVolume(box, V(0, 2, -3)));
	REQUIRE(PointInSolidVolume(box, V(std::nextafter(4.0, 0.0), 2, 2)));
	REQUIRE_FALSE(PointInSolidVolume(box, V(Up(4.0), 2, 2)));
}

TEST_CASE("PointInSolidVolume: an inward-wound box still bounds its volume", "[solid_relations]") {
	auto box = MakeSolid(BoxFaces(V(0, 0, 0), V(4, 4, 4), /*reversed=*/true));
	REQUIRE(box.validation.is_valid);
	REQUIRE(PointInSolidVolume(box, V(2, 2, 2)));
	REQUIRE_FALSE(PointInSolidVolume(box, V(2, 2, 5)));
}

TEST_CASE("PointInSolidVolume: a cavity is outside the volume", "[solid_relations]") {
	auto hollow = HollowBox();
	REQUIRE(hollow.validation.is_valid);
	REQUIRE_FALSE(PointInSolidVolume(hollow, V(2, 2, 2)));
	REQUIRE(PointInSolidVolume(hollow, V(0.5, 2, 2)));
	// Above the cavity: the ray crosses the cavity's two faces and the top.
	REQUIRE(PointInSolidVolume(hollow, V(2, 2, 0.5)));
	REQUIRE(PointInSolidVolume(hollow, V(2, 2, 3.5)));
}

TEST_CASE("PointInSolidVolume: a non-convex prism", "[solid_relations]") {
	auto l = LPrism();
	REQUIRE(l.validation.is_valid);
	REQUIRE(PointInSolidVolume(l, V(0.5, 0.5, 1)));
	REQUIRE(PointInSolidVolume(l, V(0.5, 1.5, 1)));
	REQUIRE_FALSE(PointInSolidVolume(l, V(1.5, 1.5, 1)));
	// Random points against the analytic answer.
	std::mt19937 rng(3);
	std::uniform_real_distribution<double> u(-0.5, 2.5);
	for (int i = 0; i < 2000; i++) {
		Vertex3D p = V(u(rng), u(rng), u(rng));
		bool in_prism = p.z > 0 && p.z < 2 && p.x > 0 && p.y > 0 && ((p.x < 2 && p.y < 1) || (p.x < 1 && p.y < 2));
		REQUIRE(PointInSolidVolume(l, p) == in_prism);
	}
}

TEST_CASE("Solid3DDistance: a geometry inside the volume is at distance 0", "[solid_relations]") {
	auto box = Box(V(0, 0, 0), V(4, 4, 4));
	REQUIRE(Solid3DDistance(box, Point(2, 2, 2)) == 0.0);
	REQUIRE(Solid3DDistance(box, Square(1, 1, 2, 2)) == 0.0);
	REQUIRE(Solid3DDistance(box, Point(2, 2, 6)) == Approx(2.0));
	REQUIRE(Solid3DDistance(box, Segment(V(2, 2, 2), V(2, 2, 9))) == 0.0);
	// The surface alone says otherwise.
	REQUIRE(Geom3DDistance(Square(1, 1, 2, 2), Square(0, 0, 0, 4)) == Approx(2.0));
}

TEST_CASE("Solid3DDistance: a geometry in a cavity is at its distance from the cavity walls", "[solid_relations]") {
	auto hollow = HollowBox();
	REQUIRE(Solid3DDistance(hollow, Point(2, 2, 2)) == Approx(1.0));
	REQUIRE(Solid3DDistance(hollow, Square(1.5, 1.5, 2, 1)) == Approx(0.5));
	REQUIRE(Solid3DDistance(hollow, Point(0.5, 2, 2)) == 0.0);
	REQUIRE_FALSE(Solid3DIntersects(hollow, Point(2, 2, 2)));
	REQUIRE(Solid3DIntersects(hollow, Point(0.5, 2, 2)));
	REQUIRE(Solid3DIntersects(hollow, Point(1, 2, 2))); // on the cavity wall
}

TEST_CASE("Solid3DDistance between two solids", "[solid_relations]") {
	auto big = Box(V(0, 0, 0), V(10, 10, 10));
	auto small = Box(V(4, 4, 4), V(6, 6, 6));
	auto apart = Box(V(13, 0, 0), V(15, 2, 2));
	REQUIRE(Solid3DDistance(big, small) == 0.0);
	REQUIRE(Solid3DDistance(small, big) == 0.0);
	REQUIRE(Solid3DIntersects(big, small));
	REQUIRE(Solid3DIntersects(small, big));
	REQUIRE(Solid3DDistance(big, apart) == Approx(3.0));
	REQUIRE_FALSE(Solid3DIntersects(big, apart));

	// A box in the hollow box's cavity is apart from it; one reaching into the
	// walls is not.
	auto hollow = HollowBox();
	auto in_cavity = Box(V(1.5, 1.5, 1.5), V(2.5, 2.5, 2.5));
	REQUIRE(Solid3DDistance(hollow, in_cavity) == Approx(0.5));
	REQUIRE(Solid3DDistance(in_cavity, hollow) == Approx(0.5));
	REQUIRE_FALSE(Solid3DIntersects(hollow, in_cavity));
	REQUIRE(Solid3DIntersects(hollow, Box(V(0.25, 0.25, 0.25), V(0.75, 0.75, 0.75))));
}

TEST_CASE("Solid3DIntersects is exact for touching solids", "[solid_relations]") {
	auto a = Box(V(0, 0, 0), V(1, 1, 1));
	REQUIRE(Solid3DIntersects(a, Box(V(1, 0, 0), V(2, 1, 1))));
	REQUIRE_FALSE(Solid3DIntersects(a, Box(V(Up(1.0), 0, 0), V(2, 1, 1))));
	REQUIRE(Solid3DIntersects(a, Point(1, 0.5, 0.5)));
	REQUIRE_FALSE(Solid3DIntersects(a, Point(Up(1.0), 0.5, 0.5)));
}

TEST_CASE("Solid3DWithin agrees with Solid3DDistance", "[solid_relations]") {
	auto hollow = HollowBox();
	auto in_cavity = Box(V(1.5, 1.5, 1.5), V(2.5, 2.5, 2.5));
	REQUIRE_FALSE(Solid3DWithin(hollow, in_cavity, 0.4));
	REQUIRE(Solid3DWithin(hollow, in_cavity, 0.5));
	REQUIRE(Solid3DWithin(hollow, Point(0.5, 2, 2), 0.0));
	REQUIRE_FALSE(Solid3DWithin(hollow, Point(2, 2, 2), 0.9));
	REQUIRE(Solid3DWithin(hollow, Point(2, 2, 2), 1.0));
	REQUIRE(Solid3DWithin(Box(V(0, 0, 0), V(10, 10, 10)), Point(5, 5, 5), 0.0));
}

TEST_CASE("The volume functions refuse an invalid solid", "[solid_relations]") {
	auto faces = BoxFaces(V(0, 0, 0), V(4, 4, 4));
	faces.pop_back();
	auto open = MakeSolid(faces);
	REQUIRE_FALSE(open.validation.is_valid);
	auto box = Box(V(0, 0, 0), V(1, 1, 1));
	REQUIRE_THROWS(Solid3DDistance(open, Point(2, 2, 2)));
	REQUIRE_THROWS(Solid3DWithin(open, Point(2, 2, 2), 1.0));
	REQUIRE_THROWS(Solid3DIntersects(open, Point(2, 2, 2)));
	REQUIRE_THROWS(Solid3DDistance(box, open));
	REQUIRE_THROWS(Solid3DIntersects(open, box));
}
