#pragma once

#include "kernel/core_types.hpp"

#include <cstdint>
#include <vector>
#include <array>

namespace duckdb_3d {

//! Validation flags cached in the payload
struct ValidationCache {
	uint32_t open_edge_count = 0;
	uint32_t non_manifold_edge_count = 0;
	uint32_t degenerate_face_count = 0;
	uint32_t orientation_error_count = 0;
	bool is_closed = false;
	bool is_manifold = false;
	bool is_oriented = false;
	bool is_valid = false;
};

//! The canonical in-memory solid model.
//! Preserves original polygonal topology. Triangulation is a derived cache.
struct SolidModel {
	//! Unique vertex array (deduplicated)
	std::vector<Vertex3D> vertices;

	//! Topology offset arrays (CSR-style)
	//! solid_shell_offsets[solid_count + 1] — maps solid index to shell range
	std::vector<uint32_t> solid_shell_offsets;
	//! shell_face_offsets[shell_count + 1] — maps shell index to face range
	std::vector<uint32_t> shell_face_offsets;
	//! face_ring_offsets[face_count + 1] — maps face index to ring range
	std::vector<uint32_t> face_ring_offsets;
	//! ring_vertex_offsets[ring_count + 1] — maps ring index to vertex-index range
	std::vector<uint32_t> ring_vertex_offsets;

	//! Ring vertex indices into the vertex array
	std::vector<uint32_t> ring_vertex_indices;

	//! Triangulation cache (derived)
	//! face_triangle_offsets[face_count + 1] — maps face index to triangle range
	std::vector<uint32_t> face_triangle_offsets;
	//! triangle_vertex_indices[triangle_count * 3] — triangle vertex indices
	std::vector<uint32_t> triangle_vertex_indices;

	//! Cached bounding box
	BBox3D bbox = {};

	//! Cached validation
	ValidationCache validation = {};

	//! Counts (derived from offset arrays)
	uint32_t SolidCount() const;
	uint32_t ShellCount() const;
	uint32_t FaceCount() const;
	uint32_t RingCount() const;
	uint32_t TriangleCount() const;

	//! Compute bounding box from vertices
	void ComputeBBox();
};

} // namespace duckdb_3d
