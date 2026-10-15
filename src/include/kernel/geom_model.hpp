#pragma once

#include "kernel/core_types.hpp"
#include <cstdint>
#include <string>
#include <vector>

namespace duckdb_3d {

//! General 3D geometry classes, using ISO SQL/MM WKB type codes (Z variants).
enum class GeomType : uint32_t {
	Point = 1,
	LineString = 2,
	Polygon = 3,
	MultiPoint = 4,
	MultiLineString = 5,
	MultiPolygon = 6,
	GeometryCollection = 7,
	PolyhedralSurface = 15,
};

//! A general 3D geometry model (point / line / polygon / multi / surface).
//!
//! Coordinates live in `vertices`. Structure is described by two CSR-style
//! offset arrays whose interpretation depends on `type`:
//!   - Point, LineString: `vertices` in order; offset arrays empty.
//!   - Polygon:           `ring_offsets` partitions `vertices` into rings.
//!   - Multi* / PolyhedralSurface: `part_offsets` partitions `ring_offsets`
//!     into parts, and `ring_offsets` partitions `vertices` into rings.
struct GeomModel {
	GeomType type = GeomType::Point;
	std::vector<Vertex3D> vertices;
	std::vector<uint32_t> ring_offsets;
	std::vector<uint32_t> part_offsets;
	BBox3D bbox = {};

	//! Coordinate reference system ("EPSG:28992", or any definition PROJ
	//! accepts); empty when unknown.
	std::string crs;

	//! Triangulation cache for the surface classes (Polygon, MultiPolygon,
	//! PolyhedralSurface), empty for the others. Polygon p's triangles are
	//! [polygon_triangle_offsets[p], polygon_triangle_offsets[p + 1]), each three
	//! indices into `vertices`. A polygon without area has none; its boundary is
	//! all there is of it.
	std::vector<uint32_t> polygon_triangle_offsets;
	std::vector<uint32_t> triangle_indices;

	//! Number of polygons: 1 for a Polygon with rings, the part count for a
	//! MultiPolygon or PolyhedralSurface, 0 otherwise.
	uint32_t PolygonCount() const;

	//! Polygon `polygon`'s rings: [first_ring, first_ring + ring_count) of
	//! ring_offsets' rings, the first one its exterior.
	void PolygonRings(uint32_t polygon, uint32_t &first_ring, uint32_t &ring_count) const;

	//! Recompute the bounding box from vertices.
	void ComputeBBox();
};

} // namespace duckdb_3d
