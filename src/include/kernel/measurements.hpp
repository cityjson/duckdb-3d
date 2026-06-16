#pragma once

#include "kernel/solid_model.hpp"

namespace duckdb_3d {

//! Compute total polygon surface area from the triangulation cache.
//! Requires the model to have been triangulated and have no degenerate faces.
//! Throws if preconditions are not met.
double ComputeSurfaceArea(const SolidModel &model);

//! Compute total enclosed volume using signed tetrahedral contributions.
//! Requires is_closed, is_manifold, is_oriented, and no degenerate faces.
//! Throws if preconditions are not met.
double ComputeVolume(const SolidModel &model);

//! Compute the 2D footprint area: the area of the geometry's projection onto
//! the XY plane. Computed as half the total absolute XY-projected face area,
//! which is exact for vertically-simple solids (every vertical line meets the
//! boundary once above and once below) and independent of the global
//! orientation sign. No validity preconditions beyond a parseable model.
double ComputeFootprintArea(const SolidModel &model);

//! Compute the 3D perimeter: the total length of boundary edges, i.e. edges
//! incident to exactly one face. A closed shell has no boundary edges and
//! returns 0; an open shell returns the length of its open boundary loop.
double ComputePerimeter(const SolidModel &model);

} // namespace duckdb_3d
