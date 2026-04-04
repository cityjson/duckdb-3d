#pragma once

#include "kernel/solid_model.hpp"
#include <cstdint>
#include <vector>
#include <string>
#include <stdexcept>

namespace duckdb_3d {

//! WKB geometry type codes (ISO SQL/MM)
enum class WKBGeometryType : uint32_t {
	PointZ = 1001,
	LineStringZ = 1002,
	PolygonZ = 1003,
	MultiPointZ = 1004,
	MultiLineStringZ = 1005,
	MultiPolygonZ = 1006,
	GeometryCollectionZ = 1007,
	PolyhedralSurfaceZ = 1015,
	TINZ = 1016,
	TriangleZ = 1017,
};

//! Result of parsing WKB into raw polygon topology (before canonical model construction).
//! One ParsedPolyhedralSurface per PolyhedralSurface Z found.
struct ParsedPolyhedralSurface {
	//! All vertices in order as they appear in WKB rings
	std::vector<Vertex3D> vertices;
	//! Number of polygons (faces)
	uint32_t polygon_count = 0;
	//! For each polygon, number of rings
	std::vector<uint32_t> polygon_ring_counts;
	//! For each ring, the vertex indices (start index into vertices, count)
	//! Stored as [start, count] pairs flattened
	std::vector<uint32_t> ring_vertex_counts;
};

//! Parse WKB bytes into a list of ParsedPolyhedralSurface.
//! Supports PolyhedralSurface Z and GeometryCollection Z of PolyhedralSurface Z.
//! Throws std::runtime_error for unsupported or malformed WKB.
std::vector<ParsedPolyhedralSurface> ParseWKB(const uint8_t *data, size_t size);

} // namespace duckdb_3d
