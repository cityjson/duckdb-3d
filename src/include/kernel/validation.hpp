#pragma once

#include "kernel/solid_model.hpp"

namespace duckdb_3d {

//! Run all validation checks on a SolidModel and populate its ValidationCache.
//! This analyzes edge topology per shell for closedness, manifoldness,
//! and orientation consistency.
void ValidateSolidModel(SolidModel &model);

} // namespace duckdb_3d
