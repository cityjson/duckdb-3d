#pragma once

#include "kernel/solid_model.hpp"
#include "kernel/wkb_parser.hpp"
#include "kernel/metadata_parser.hpp"
#include <vector>

namespace duckdb_3d {

//! Build a canonical SolidModel from parsed WKB surfaces.
//! Each ParsedPolyhedralSurface becomes one solid with one shell (plain WKB import).
//! Deduplicates vertices and builds all offset arrays.
SolidModel BuildSolidModel(const std::vector<ParsedPolyhedralSurface> &surfaces);

//! Build a canonical SolidModel with metadata-aware shell grouping.
//! When metadata provides shellCount > 1 and shellFaceCounts, the faces of a
//! single PolyhedralSurface are split into multiple shells per solid.
//! Throws if metadata conflicts with WKB face counts.
SolidModel BuildSolidModel(const std::vector<ParsedPolyhedralSurface> &surfaces,
                           const GeometryMetadata &metadata);

} // namespace duckdb_3d
