#pragma once

#include "kernel/solid_model.hpp"

namespace duckdb_3d {

//! Unnormalised Newell area vector of ring `ring_idx` of `model`. Its
//! magnitude is twice the ring's area; its direction follows the right-hand
//! rule around the ring's winding. Zero for rings with fewer than 3 vertices.
Vertex3D NewellRingAreaVector(const SolidModel &model, uint32_t ring_idx);

//! True when a ring's trailing vertex repeats its first. WKB closes every ring
//! explicitly; the canonical models do not store the duplicate, so both WKB
//! parsers drop it. Exact (bitwise-value) comparison — no tolerance.
inline bool IsClosingVertex(const Vertex3D &first, const Vertex3D &last) {
	return first == last;
}

} // namespace duckdb_3d
