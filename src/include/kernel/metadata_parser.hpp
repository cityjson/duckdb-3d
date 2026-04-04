#pragma once

#include <cstdint>
#include <vector>
#include <string>

namespace duckdb_3d {

//! Parsed CityJSON geometry_properties metadata relevant to shell grouping.
struct GeometryMetadata {
	//! CityJSON geometry type: "Solid", "MultiSolid", "CompositeSolid", etc.
	std::string type;
	//! Number of shells per solid (for "Solid" type).
	//! If > 1, the faces in the PolyhedralSurface are split into shells.
	uint32_t shell_count = 1;
	//! Number of solids (for GeometryCollection-backed types).
	uint32_t solid_count = 1;
	//! Face counts per shell (if available). Used to split face ranges.
	//! When non-empty, sum must equal total face count from WKB.
	std::vector<uint32_t> shell_face_counts;
};

//! Parse geometry_properties JSON text into GeometryMetadata.
//! Throws std::runtime_error if the JSON is malformed or contains
//! conflicting information.
GeometryMetadata ParseGeometryProperties(const std::string &json_text);

} // namespace duckdb_3d
