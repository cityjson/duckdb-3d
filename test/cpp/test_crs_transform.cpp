#include "catch.hpp"
#include "kernel/crs_transform.hpp"
#include <cmath>
#include <vector>

using namespace duckdb_3d;

TEST_CASE("EpsgToAuthString formats EPSG codes", "[crs_transform]") {
	REQUIRE(EpsgToAuthString(4326) == "EPSG:4326");
	REQUIRE(EpsgToAuthString(28992) == "EPSG:28992");
}

TEST_CASE("EpsgToAuthString rejects non-positive SRID", "[crs_transform]") {
	REQUIRE_THROWS(EpsgToAuthString(0));
	REQUIRE_THROWS(EpsgToAuthString(-1));
}

TEST_CASE("CrsTransform 4326->3857 maps origin to origin and preserves Z", "[crs_transform]") {
	CrsTransform tf("EPSG:4326", "EPSG:3857");
	std::vector<Vertex3D> v = {{0.0, 0.0, 42.0}};
	tf.ReprojectXY(v);
	REQUIRE(std::abs(v[0].x) < 1e-6);
	REQUIRE(std::abs(v[0].y) < 1e-6);
	REQUIRE(v[0].z == 42.0); // Z untouched — no vertical datum
}

TEST_CASE("CrsTransform 4326->3857 uses lon/lat axis order and known easting", "[crs_transform]") {
	CrsTransform tf("EPSG:4326", "EPSG:3857");
	// With visualization-normalised axis order, input is (lon, lat).
	std::vector<Vertex3D> v = {{8.0, 0.0, 0.0}};
	tf.ReprojectXY(v);
	// Web Mercator easting = lon * (20037508.342789244 / 180).
	REQUIRE(v[0].x == Approx(8.0 * 20037508.342789244 / 180.0).epsilon(1e-6));
	REQUIRE(std::abs(v[0].y) < 1e-3);
}

TEST_CASE("CrsTransform round-trips 4326->28992->4326", "[crs_transform]") {
	// A point over the Netherlands (near Delft): lon 4.36, lat 52.0.
	std::vector<Vertex3D> orig = {{4.36, 52.0, 7.5}};
	std::vector<Vertex3D> v = orig;

	CrsTransform fwd("EPSG:4326", "EPSG:28992");
	fwd.ReprojectXY(v);
	// Sanity: it actually moved into RD-New metric range.
	REQUIRE(v[0].x > 0.0);
	REQUIRE(v[0].x < 300000.0);

	CrsTransform back("EPSG:28992", "EPSG:4326");
	back.ReprojectXY(v);
	REQUIRE(v[0].x == Approx(orig[0].x).epsilon(1e-6));
	REQUIRE(v[0].y == Approx(orig[0].y).epsilon(1e-6));
	REQUIRE(v[0].z == orig[0].z); // Z preserved across the whole round-trip
}

TEST_CASE("CrsTransform throws on an unknown CRS", "[crs_transform]") {
	REQUIRE_THROWS(CrsTransform("EPSG:99999999", "EPSG:3857"));
}

// NOTE: the documented contract requires each argument to be a CRS description
// (authority code / WKT2). PROJ's handling of a coordinate-operation pipeline in
// a CRS position is version-dependent (it does not reliably reject one), so we
// deliberately do not assert on that input — it is unsupported, not guaranteed
// to throw. See kernel/crs_transform.hpp.
