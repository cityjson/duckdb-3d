#pragma once

#include "kernel/geom_model.hpp"
#include <string>
#include <vector>

namespace duckdb_3d {

//! ISO WKT output (with Z) for a GEOM_3D model.
std::string Geom3DAsText(const GeomModel &geom);

//! GeoJSON output for a GEOM_3D model.
std::string Geom3DAsGeoJSON(const GeomModel &geom);

//! OGC/ISO WKB output for a GEOM_3D model (little-endian, Z variants).
std::vector<uint8_t> Geom3DAsBinary(const GeomModel &geom);

} // namespace duckdb_3d
