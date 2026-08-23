#pragma once

#include "kernel/core_types.hpp"
#include "kernel/solid_model.hpp"

namespace duckdb_3d {

//! Unnormalised Newell area vector of ring `ring_idx` of `model`. Its
//! magnitude is twice the ring's area; its direction follows the right-hand
//! rule around the ring's winding. Zero for rings with fewer than 3 vertices.
Vertex3D NewellRingAreaVector(const SolidModel &model, uint32_t ring_idx);

//! Local reference point for a shell's divergence-theorem volume sum: the
//! shell's first triangulated vertex. Writes it to `out` and returns true;
//! returns false (leaving `out` untouched) when the shell has no triangles, so
//! there is nothing to integrate.
//!
//! The reference point MUST be hoisted per shell, not per model. Summing
//! a·(b×c) about a point far from the shell makes the intermediate products
//! scale as |distance|^3 while the answer scales as |extent|^3, so almost every
//! significant digit cancels — the failure this exists to prevent. A model-wide
//! reference point is only close to its shells when the model is compact; for a
//! MultiSolid/CompositeSolid with separated parts it is far from every one of
//! them. Translation does not change a closed shell's signed volume, so giving
//! each shell its own reference point is exact, and it keeps validation.cpp's
//! winding check and measurements.cpp's volume in bit-for-bit agreement.
bool ShellLocalOrigin(const SolidModel &model, uint32_t shell_idx, Vertex3D &out);

//! True when a ring's trailing vertex repeats its first. WKB closes every ring
//! explicitly; the canonical models do not store the duplicate, so both WKB
//! parsers drop it. Exact (bitwise-value) comparison — no tolerance.
inline bool IsClosingVertex(const Vertex3D &first, const Vertex3D &last) {
	return first == last;
}

} // namespace duckdb_3d
