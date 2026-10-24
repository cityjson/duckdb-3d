#pragma once

#include "kernel/core_types.hpp"
#include "kernel/geom_model.hpp"
#include "kernel/solid_model.hpp"

#include <cstdint>
#include <memory>
#include <vector>

namespace duckdb_3d {

//! Triangulate all faces in the solid model and populate the triangulation cache:
//! face_triangle_offsets and triangle_vertex_indices.
//! Each face is triangulated whole (holes bridged into the exterior) in its own
//! plane, every triangle wound like the exterior ring and every ring vertex used.
//! A face whose triangles would not tile it keeps an empty triangle range, never
//! a partial one; validation counts such a face degenerate.
void TriangulateSolidModel(SolidModel &model);

//! Fill a GEOM_3D model's triangulation cache (polygon_triangle_offsets,
//! triangle_indices) for its surface classes; other classes get empty arrays.
//! A polygon without area gets no triangles. Throws std::invalid_argument when a
//! polygon admits no tiling (a ring that crosses itself, a hole outside the
//! exterior): such a polygon has no interior to measure against.
void TriangulateGeomModel(GeomModel &model);

//! What triangulating one polygon produced.
enum class PolygonTriangulation : uint8_t {
	//! Triangles appended; together they tile the polygon.
	Tiled,
	//! Nothing to tile: a ring with fewer than three vertices, or no area at all.
	//! Nothing appended.
	ZeroArea,
	//! The rings admit no tiling: a ring that crosses itself, a hole outside the
	//! exterior, or coordinates whose areas are not finite. Nothing appended.
	Untileable,
};

//! Triangulates polygons one at a time, keeping its working buffers between
//! calls so a caller with many faces allocates once rather than per face. The
//! method TriangulateSolidModel uses for each face: the polygon is projected into
//! its own plane, triangulated whole with its holes bridged into the exterior,
//! and accepted only if the triangles tile it.
class PolygonTriangulator {
public:
	PolygonTriangulator();
	~PolygonTriangulator();
	PolygonTriangulator(const PolygonTriangulator &) = delete;
	PolygonTriangulator &operator=(const PolygonTriangulator &) = delete;

	//! Triangulate the polygon whose ring r is the vertex run
	//! [ring_offsets[r], ring_offsets[r + 1]) of `ring_vertex_indices`, for r in
	//! [0, ring_count); ring 0 is the exterior and the rest are holes. A null
	//! `ring_vertex_indices` means a run's positions index `vertices` directly.
	//! Rings are open: the closing vertex is not repeated. On Tiled, appends each
	//! triangle's three indices into `vertices` to `out`, wound like the exterior
	//! ring, and every ring vertex is used.
	PolygonTriangulation Triangulate(const Vertex3D *vertices, const uint32_t *ring_vertex_indices,
	                                 const uint32_t *ring_offsets, uint32_t ring_count, std::vector<uint32_t> &out);

	//! The working buffers; opaque outside triangulation.cpp.
	struct Scratch;

private:
	std::unique_ptr<Scratch> scratch;
};

} // namespace duckdb_3d
