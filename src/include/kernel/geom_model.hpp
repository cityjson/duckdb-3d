#pragma once

#include "kernel/solid_model.hpp" // Vertex3D, BBox3D
#include <cstdint>
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

	//! Recompute the bounding box from vertices.
	void ComputeBBox();
};

} // namespace duckdb_3d
