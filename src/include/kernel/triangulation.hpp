#pragma once

#include "kernel/solid_model.hpp"

namespace duckdb_3d {

//! Triangulate all faces in the solid model and populate the triangulation cache:
//! face_triangle_offsets and triangle_vertex_indices.
//! Each face is triangulated whole (holes bridged into the exterior) in its own
//! plane, every triangle wound like the exterior ring and every ring vertex used.
//! A face whose triangles would not tile it keeps an empty triangle range, never
//! a partial one; validation counts such a face degenerate.
void TriangulateSolidModel(SolidModel &model);

} // namespace duckdb_3d
