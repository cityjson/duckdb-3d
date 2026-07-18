#pragma once

#include "kernel/geom_model.hpp"
#include "kernel/solid_model.hpp" // Vertex3D

namespace duckdb_3d {

//! Whether every face/ring of the geometry is planar within tolerance. Points and
//! single segments are trivially planar; for surfaces each face is checked.
bool Geom3DIsPlanar(const GeomModel &geom);

//! The 3D centroid of the geometry: vertex average for points, length-weighted
//! midpoint for lines, area-weighted centroid for polygonal/surface geometries.
Vertex3D Geom3DCentroid(const GeomModel &geom);

//! 2D convex hull over the XY projection of all vertices. Returns a Polygon Z
//! (or LineString Z / Point Z for degenerate inputs) at the minimum Z of the input.
GeomModel Geom3DConvexHull(const GeomModel &geom);

//! Area of the geometry's XY projection (footprint). Polygon/MultiPolygon/
//! PolyhedralSurface only; exterior rings add and interior rings subtract.
//! Non-areal geometries (points, lines) and vertical faces project to 0.
double Geom3DFootprintArea(const GeomModel &geom);

} // namespace duckdb_3d
