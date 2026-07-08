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

//! Lower bound on the minimum distance between two geometries, computed from
//! their axis-aligned bounding boxes alone (0 when the boxes overlap). Since
//! every geometry lies inside its bbox, this never exceeds Geom3DDistance, so
//! it is a sound early-rejection bound for `within`/distance-threshold queries.
double Geom3DBBoxDistance(const GeomModel &g1, const GeomModel &g2);

//! True when the minimum distance between `g1` and `g2` is <= `threshold`.
//! Equivalent to `Geom3DDistance(g1, g2) <= threshold` but cheaper: it rejects
//! early when the bbox lower bound already exceeds the threshold, and stops at
//! the first element pair found within the threshold instead of computing the
//! exact minimum.
bool Geom3DWithin(const GeomModel &g1, const GeomModel &g2, double threshold);

//! Maximum 3D distance between two general geometries (attained at vertices).
double Geom3DMaxDistance(const GeomModel &g1, const GeomModel &g2);

//! Pair of closest points (p on g1, q on g2) realising the minimum distance.
//! When the geometries intersect/touch, p and q coincide.
struct ClosestPointPair {
	Vertex3D p;
	Vertex3D q;
};

ClosestPointPair Geom3DClosestPoints(const GeomModel &g1, const GeomModel &g2);

} // namespace duckdb_3d
