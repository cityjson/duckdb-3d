#pragma once

#include "kernel/solid_model.hpp"
#include "kernel/wkb_parser.hpp"
#include <vector>

namespace duckdb_3d {

//! Build a canonical SolidModel from parsed WKB surfaces.
//! Each ParsedPolyhedralSurface becomes one solid with one shell (plain WKB import).
//! Deduplicates vertices and builds all offset arrays.
SolidModel BuildSolidModel(const std::vector<ParsedPolyhedralSurface> &surfaces);

} // namespace duckdb_3d
