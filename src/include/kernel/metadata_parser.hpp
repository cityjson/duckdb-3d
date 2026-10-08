#pragma once

#include <cstdint>
#include <optional>
#include <vector>
#include <string>
#include <string_view>

namespace duckdb_3d {

//! Parsed geometry_properties metadata relevant to shell grouping. The WKB flattens a solid's shells into one flat face
//! list, so the shell partition is recovered from the `shells` key here.
//! Per-solid, per-shell face counts.
using ShellCounts = std::vector<std::vector<uint32_t>>;

struct GeometryMetadata {
	//! Geometry type string: "Solid", "MultiSolid", "CompositeSolid", …
	std::string type;
	//! Per-solid, per-shell face counts (`shells`, LIST<LIST<INT>>):
	//!   Solid                    -> {{12}} or {{12, 4}}   (one solid)
	//!   MultiSolid/CompositeSolid -> {{12}, {8, 4}}        (one array per solid)
	//! Absent when the sidecar carries no `shells`, or a null one (the non-solid
	//! types); the builder then falls back to one solid / one shell per WKB
	//! member. A present value, even an empty one, must describe every member.
	std::optional<ShellCounts> shells;
};

//! Parse geometry_properties JSON text into GeometryMetadata.
//! Throws std::runtime_error if the JSON is malformed or contains
//! conflicting information.
GeometryMetadata ParseGeometryProperties(std::string_view json_text);

} // namespace duckdb_3d
