#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace duckdb_3d {

//! The ISO WKB of a 3D WKT geometry, byte for byte what PostGIS's
//! ST_AsBinary(geom, 'NDR') gives for it, so a GEOM_3D read from either is the
//! same value. Accepts POINT, LINESTRING, POLYGON, MULTIPOINT, MULTILINESTRING,
//! MULTIPOLYGON, POLYHEDRALSURFACE, TIN and TRIANGLE, case-insensitive, tagged Z
//! or untagged with three ordinates, and an EWKT `SRID=<n>;` prefix, which
//! becomes an EWKB SRID. Throws std::invalid_argument, naming the offset, for
//! 2D or M coordinates, any other class, and malformed text.
std::vector<uint8_t> WktToWkb(const char *text, size_t size);

} // namespace duckdb_3d
