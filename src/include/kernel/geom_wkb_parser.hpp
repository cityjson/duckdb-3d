#pragma once

#include "kernel/geom_model.hpp"
#include <cstddef>
#include <cstdint>

namespace duckdb_3d {

//! Parse an ISO SQL/MM WKB blob into a general GeomModel.
//! Supported in this slice: Point Z. Other classes throw std::runtime_error
//! until added. Big-endian WKB is rejected for now.
GeomModel ParseGeomWKB(const uint8_t *data, size_t size);

} // namespace duckdb_3d
