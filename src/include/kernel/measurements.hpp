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

} // namespace duckdb_3d
