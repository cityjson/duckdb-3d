#pragma once

#include "kernel/core_types.hpp"
#include "kernel/geom_model.hpp"

namespace duckdb_3d {

// Exact intersection tests. Every decision is a sign of Shewchuk's adaptive
// orient2d/orient3d or a comparison of input coordinates, so the answer is
// that of the closed primitives with the given double coordinates: touching
// counts, and a gap of one unit in the last place does not.

//! Whether point p lies on the closed segment [a, b] (a point when a == b).
bool PointOnSegment(const Vertex3D &p, const Vertex3D &a, const Vertex3D &b);

//! Whether the closed segments [p1, q1] and [p2, q2] share a point.
bool SegmentsIntersect(const Vertex3D &p1, const Vertex3D &q1, const Vertex3D &p2, const Vertex3D &q2);

//! Whether point p lies on the closed triangle (a, b, c). A triangle without
//! area is the union of its edges.
bool PointInTriangle(const Vertex3D &p, const Vertex3D &a, const Vertex3D &b, const Vertex3D &c);

//! Whether the closed segment [p, q] and the closed triangle (a, b, c) share a
//! point.
bool SegmentTriangleIntersect(const Vertex3D &p, const Vertex3D &q, const Vertex3D &a, const Vertex3D &b,
                              const Vertex3D &c);

//! Whether two closed triangles share a point.
bool TrianglesIntersect(const Vertex3D &a1, const Vertex3D &b1, const Vertex3D &c1, const Vertex3D &a2,
                        const Vertex3D &b2, const Vertex3D &c2);

//! Whether two geometries share a point, decided exactly on the elements
//! Geom3DDistance measures (points, segments, and the triangles that tile each
//! polygon, holes excluded).
bool Geom3DIntersects(const GeomModel &g1, const GeomModel &g2);

} // namespace duckdb_3d
