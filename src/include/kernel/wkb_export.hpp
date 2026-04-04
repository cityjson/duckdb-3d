#pragma once

#include "kernel/solid_model.hpp"
#include <cstdint>
#include <vector>

namespace duckdb_3d {

//! Export a SolidModel to WKB format.
//! Single-solid → PolyhedralSurface Z
//! Multi-solid → GeometryCollection Z of PolyhedralSurface Z
std::vector<uint8_t> ExportWKB(const SolidModel &model);

} // namespace duckdb_3d
