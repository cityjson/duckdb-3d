#pragma once

#include <cstdint>
#include <vector>
#include <string>

namespace duckdb_3d {

//! Parsed CityParquet spec §8 geometry_properties metadata relevant to shell
//! grouping. The WKB flattens a solid's shells into one flat face list, so the
//! shell partition is recovered from the `shells` key here.
struct GeometryMetadata {
	//! CityJSON geometry type string: "Solid", "MultiSolid", "CompositeSolid", …
	std::string type;
	//! Per-solid, per-shell emitted-face counts (spec §8 `shells`):
	//!   Solid                    -> {{12}} or {{12, 4}}   (one solid)
	//!   MultiSolid/CompositeSolid -> {{12}, {8, 4}}        (one array per solid)
	//! Empty when the geometry carries no `shells` (non-solid types); the builder
	//! then falls back to one solid / one shell per WKB member.
	std::vector<std::vector<uint32_t>> shells;
};

//! Parse geometry_properties JSON text into GeometryMetadata.
//! Throws std::runtime_error if the JSON is malformed or contains
//! conflicting information.
GeometryMetadata ParseGeometryProperties(const std::string &json_text);

} // namespace duckdb_3d
