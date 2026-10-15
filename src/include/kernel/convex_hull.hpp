#pragma once

#include "kernel/core_types.hpp"
#include "kernel/geom_model.hpp"

#include <vector>

namespace duckdb_3d {

//! The 3D convex hull of a point set, computed with exact orientation
//! predicates, so its vertices are input points and its combinatorics are
//! exact. The result has the dimension of the input's affine hull:
//!   - no points: an empty Point; one distinct point: that Point;
//!   - collinear points: the LineString between the two extreme points;
//!   - coplanar points: a Polygon, one ring without collinear vertices, wound
//!     counterclockwise seen from above (from +y for a vertical plane, from +x
//!     for a plane parallel to both y and z);
//!   - otherwise a closed PolyhedralSurface of convex polygons, one per
//!     supporting plane, each wound counterclockwise seen from outside (outward
//!     right-handed normals), with no collinear vertices.
//! The triangulation cache is left empty; serialising the model fills it.
GeomModel ConvexHull3D(const std::vector<Vertex3D> &points);

} // namespace duckdb_3d
