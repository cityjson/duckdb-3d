#pragma once

#include "kernel/solid_model.hpp"

namespace duckdb_3d {

//! Triangulate all faces in the solid model and populate the triangulation cache:
//! face_triangle_offsets and triangle_vertex_indices.
//! Uses ear-clipping after projecting each face to 2D via its face normal.
void TriangulateSolidModel(SolidModel &model);

} // namespace duckdb_3d
