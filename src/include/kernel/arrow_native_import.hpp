#pragma once

#include "kernel/solid_model.hpp"
#include <cstdint>
#include <vector>

namespace duckdb_3d {

//! Plain-C++, DuckDB-agnostic flattened representation of the arrow-native
//! nested boundaries shape (solid -> shell -> face -> ring -> index), CSR
//! offset arrays over a flat vertex-index buffer. The DuckDB-aware SQL layer
//! (three_d_extension.cpp) walks the nested LIST<...<LIST<INTEGER>>> Vector
//! and produces this; this type — and everything below it in this file —
//! never sees a DuckDB Vector directly, keeping kernel/ DuckDB-free (mirrors
//! how ParseWKB only ever sees a flat uint8_t*/size, never a Vector).
//!
//! Both the Solid/MultiSolid/CompositeSolid family (BuildSolidModelFromArrowNative)
//! and the padded MultiSurface/CompositeSurface family (BuildGeomModelFromArrowNative)
//! share this exact same flattened shape — a surface value is a boundaries
//! value with solid_shell_offsets/shell_face_offsets padded to size 2 (one
//! solid, one shell), per the design doc's padding-dimension convention.
struct ArrowNativeBoundaries {
	std::vector<uint32_t> solid_shell_offsets; // [solid_count+1]
	std::vector<uint32_t> shell_face_offsets;  // [shell_count+1]
	std::vector<uint32_t> face_ring_offsets;   // [face_count+1]
	std::vector<uint32_t> ring_vertex_offsets; // [ring_count+1]
	std::vector<uint32_t> ring_vertex_indices; // raw indices into the vertex pool
};

//! Builds a validated, triangulated SolidModel from already-flattened
//! arrow-native boundaries + a vertex pool. This is a public SQL-callable
//! ingestion boundary, so — unlike simply trusting the writer's
//! distinct-index-compaction invariant — vertices are defensively
//! deduplicated by coordinate equality (mirroring model_builder.cpp's own
//! GetOrAddVertex) before indices are remapped and bounds-checked against
//! them; SolidModel's own construction pipeline, shared with the WKB path,
//! still runs in full: ComputeBBox, TriangulateSolidModel, ValidateSolidModel.
SolidModel BuildSolidModelFromArrowNative(const ArrowNativeBoundaries &boundaries,
                                          const std::vector<Vertex3D> &vertices);

} // namespace duckdb_3d
