#pragma once

#include "kernel/core_types.hpp"
#include "kernel/geom_model.hpp"
#include "kernel/solid_model.hpp"

namespace duckdb_3d {

// A SOLID_3D in the distance family is the volume its shells bound, cavities
// excluded: a geometry inside it is at distance 0 from it and intersects it.
// That volume is defined only for a valid solid (ValidationCache::is_valid), so
// every function here throws std::runtime_error for an invalid one.

//! Whether p lies inside the solid's volume, decided exactly: the signed number
//! of the solid's triangles a ray from p crosses, with ties broken by symbolic
//! perturbation. A point on the surface is outside the scope of this test (the
//! callers decide those with the surface first); the answer for it is either.
bool PointInSolidVolume(const SolidModel &solid, const Vertex3D &p);

//! Minimum 3D distance between the solid's volume and a geometry (0 when any
//! part of the geometry lies inside the volume).
double Solid3DDistance(const SolidModel &solid, const GeomModel &geom);
//! Minimum 3D distance between two solids' volumes (0 when they overlap or one
//! contains the other).
double Solid3DDistance(const SolidModel &a, const SolidModel &b);

//! Whether the distance above is at most `threshold`.
bool Solid3DWithin(const SolidModel &solid, const GeomModel &geom, double threshold);
bool Solid3DWithin(const SolidModel &a, const SolidModel &b, double threshold);

//! Whether the solid's volume and the geometry share a point, decided exactly.
bool Solid3DIntersects(const SolidModel &solid, const GeomModel &geom);
bool Solid3DIntersects(const SolidModel &a, const SolidModel &b);

} // namespace duckdb_3d
