#pragma once

#include "kernel/geom_model.hpp"
#include "kernel/solid_model.hpp"

namespace duckdb_3d {

//! Build a vertical-prism SOLID_3D model by extruding a Polygon GEOM_3D footprint
//! upward by `height`. Bottom face = footprint, top face = footprint translated by
//! (0,0,height), side faces connect corresponding edges. The result is a closed,
//! oriented, manifold one-shell solid.
//!
//! Throws std::runtime_error if the geometry is not a Polygon, has fewer than three
//! distinct exterior-ring vertices, or if `height <= 0`.
SolidModel BuildExtrudedSolid(const GeomModel &polygon, double height);

} // namespace duckdb_3d
