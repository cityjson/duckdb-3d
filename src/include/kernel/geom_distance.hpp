#pragma once

#include "kernel/geom_model.hpp"
#include "kernel/solid_model.hpp" // Vertex3D

namespace duckdb_3d {

// --- Primitive 3D distance helpers (exact, no triangulation cache needed) ---

//! Distance between two points.
double DistPointPoint(const Vertex3D &p, const Vertex3D &q);

//! Distance between point `p` and segment [a,b].
double DistPointSegment(const Vertex3D &p, const Vertex3D &a, const Vertex3D &b);

//! Distance between segments [p1,q1] and [p2,q2].
double DistSegmentSegment(const Vertex3D &p1, const Vertex3D &q1, const Vertex3D &p2,
                          const Vertex3D &q2);

//! Distance between point `p` and triangle (a,b,c).
double DistPointTriangle(const Vertex3D &p, const Vertex3D &a, const Vertex3D &b,
                         const Vertex3D &c);

//! Distance between segment [p,q] and triangle (a,b,c). 0 if they intersect.
double DistSegmentTriangle(const Vertex3D &p, const Vertex3D &q, const Vertex3D &a,
                           const Vertex3D &b, const Vertex3D &c);

//! Distance between triangles (a1,b1,c1) and (a2,b2,c2). 0 if they intersect.
double DistTriangleTriangle(const Vertex3D &a1, const Vertex3D &b1, const Vertex3D &c1,
                            const Vertex3D &a2, const Vertex3D &b2, const Vertex3D &c2);

//! Minimum 3D distance between two general geometries. 0 if they intersect.
double Geom3DDistance(const GeomModel &g1, const GeomModel &g2);

//! Maximum 3D distance between two general geometries (attained at vertices).
double Geom3DMaxDistance(const GeomModel &g1, const GeomModel &g2);

} // namespace duckdb_3d
