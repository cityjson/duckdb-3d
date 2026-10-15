#pragma once

#include "kernel/solid_model.hpp"
#include <cstdint>

namespace duckdb_3d {

//! Number of face pairs of `model` that cross each other, counted per solid
//! over all its shells (an interior shell crossing the exterior counts). Uses
//! the triangulation cache; faces without triangles are skipped.
//!
//! Faces that only touch (a shared edge or vertex, a coplanar neighbour) do
//! not count. A pair of triangles from different faces counts when:
//!   - one lies within the rounding term (kEpsRelative times the solid's
//!     bounding-box diagonal) of the other's face plane, and their overlap in
//!     that plane is wider (twice the area over the perimeter) than the
//!     tolerance; or
//!   - each reaches further than the tolerance to both sides of the other's
//!     face plane, the two segments where each triangle meets the other's
//!     triangle plane overlap by more than the tolerance, and that overlap
//!     lies further than the tolerance from the boundary of one of the faces.
//! Depth is measured against face planes, so a sliver triangle, whose own
//! plane is ill-defined, cannot make a neighbour look deep; where the
//! triangles cross is measured on the triangles, whose shared vertices lie
//! exactly on both planes. A crossing within the tolerance of both faces'
//! boundaries is a fold of the junction where they meet, which another
//! triangulation of the same rings need not have.
//!
//! The tolerance for a pair is twice the sum of the two faces' deviations
//! from their own planes, plus 1e-5 times the solid's bounding-box diagonal
//! (half a millimetre on a building 50 m across: city models publish
//! millimetre coordinates, so two faces meeting at a vertex can cross by that
//! much for no other reason). It has no unit. Every
//! triangulation of a face stays within its deviation of its plane, so two
//! triangulations of the same face can be up to twice that apart: a crossing
//! shallower than the tolerance depends on how the non-planar faces were
//! triangulated, and is not counted as a defect of the solid. Faces closer
//! than the tolerance that do not cross are not counted either.
uint32_t CountSelfIntersections(const SolidModel &model);

} // namespace duckdb_3d
