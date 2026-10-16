#pragma once

#include "kernel/solid_model.hpp"

namespace duckdb_3d {

//! Run all validation checks on a SolidModel and populate its ValidationCache:
//! per shell, closedness, edge and vertex manifoldness, orientation and
//! connectivity; per solid, interior-shell winding and faces crossing each
//! other (kernel/self_intersection.hpp). The last two need the triangulation.
void ValidateSolidModel(SolidModel &model);

} // namespace duckdb_3d
