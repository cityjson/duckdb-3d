#pragma once

#include "kernel/geom_model.hpp"
#include "kernel/metadata_parser.hpp"
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

//! Metadata-aware overload: regroups the physical boundaries' shells from
//! `metadata.shells` before delegating to the 2-arg overload above, mirroring
//! model_builder.cpp's BuildSolidModel(surfaces, metadata) exactly. Real
//! arrow-native producers (confirmed: cityparquet-rs's arrow_geom_write.rs)
//! always pad each solid to exactly one physical shell, flattening any real
//! interior shells into a single face list exactly like the WKB path
//! flattens them into one PolyhedralSurface — the real per-solid shell
//! partition lives only in geometry_properties.shells, never in the
//! boundaries' own "shell" nesting level for the Solid family. Skipping this
//! regrouping would silently merge an exterior and interior shell into one,
//! so CheckInteriorShellWinding never runs and a same-wound (invalid) cavity
//! would be accepted with its volume wrongly added instead of subtracted.
//! If `metadata.shells` is empty, delegates unchanged (matches
//! BuildSolidModel(surfaces) with no metadata: whatever grouping the
//! boundaries already have is used as-is).
SolidModel BuildSolidModelFromArrowNative(const ArrowNativeBoundaries &boundaries,
                                          const std::vector<Vertex3D> &vertices, const GeometryMetadata &metadata);

//! Builds a GeomModel (MultiPolygon Z family) from a padded (solid-count 1,
//! shell-count 1) ArrowNativeBoundaries + a vertex pool. Unlike
//! BuildSolidModelFromArrowNative, GeomModel is not index-based — ring
//! indices are dereferenced and expanded into inline coordinates, not copied.
//! Throws if the padding-dimension invariant doesn't hold (a real
//! multi-shell/multi-solid value was passed where a surface value was
//! expected — call BuildSolidModelFromArrowNative for that) or if an index
//! is out of range.
GeomModel BuildGeomModelFromArrowNative(const ArrowNativeBoundaries &boundaries, const std::vector<Vertex3D> &vertices);

} // namespace duckdb_3d
