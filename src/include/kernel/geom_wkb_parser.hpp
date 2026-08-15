#pragma once

#include "kernel/geom_model.hpp"
#include <cstddef>
#include <cstdint>

namespace duckdb_3d {

//! Parse an ISO SQL/MM WKB blob into a general GeomModel. Supported classes:
//! Point/LineString/Polygon/MultiPoint/MultiLineString/MultiPolygon/
//! PolyhedralSurface (Z variants), in either byte order; unsupported type
//! codes throw std::runtime_error.
GeomModel ParseGeomWKB(const uint8_t *data, size_t size);

} // namespace duckdb_3d
