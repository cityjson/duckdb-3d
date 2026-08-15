#pragma once

#include "kernel/solid_model.hpp"

namespace duckdb_3d {

//! Unnormalised Newell area vector of ring `ring_idx` of `model`. Its
//! magnitude is twice the ring's area; its direction follows the right-hand
//! rule around the ring's winding. Zero for rings with fewer than 3 vertices.
Vertex3D NewellRingAreaVector(const SolidModel &model, uint32_t ring_idx);

} // namespace duckdb_3d
