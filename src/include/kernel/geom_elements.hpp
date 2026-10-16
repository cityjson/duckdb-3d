#pragma once

#include "kernel/core_types.hpp"
#include "kernel/geom_model.hpp"
#include "kernel/solid_model.hpp"

#include <vector>

namespace duckdb_3d {

//! A primitive the distance and intersection sweeps work on: a point (n = 1), a
//! segment (n = 2) or a triangle (n = 3), with its bounding box.
struct Element {
	int n;
	Vertex3D v[3];
	BBox3D box;
};

Element MakeElement(int n, const Vertex3D &a, const Vertex3D &b = {}, const Vertex3D &c = {});

//! A geometry as elements, by dimension: points, segments, and for polygons and
//! surface patches the triangles that tile them, holes excluded (the cache every
//! GEOM_3D value carries, or computed here for a model built in memory without
//! one, which throws for a polygon that admits no tiling). A polygon with no area
//! contributes its boundary edges.
std::vector<Element> Decompose(const GeomModel &m);

//! A solid's surface as elements: its cached triangles.
std::vector<Element> Decompose(const SolidModel &m);

//! Minimum distance over every element pair; infinity when either side is
//! empty. `box1` and `box2` bound the two element sets.
double ElementsDistance(const std::vector<Element> &e1, const BBox3D &box1, const std::vector<Element> &e2,
                        const BBox3D &box2);

//! Whether some element pair is within `threshold`.
bool ElementsWithin(const std::vector<Element> &e1, const BBox3D &box1, const std::vector<Element> &e2,
                    const BBox3D &box2, double threshold);

//! Whether some element pair shares a point, decided exactly.
bool ElementsIntersect(const std::vector<Element> &e1, const BBox3D &box1, const std::vector<Element> &e2,
                       const BBox3D &box2);

//! Whether two closed boxes share a point (exact).
bool BoxesOverlap(const BBox3D &a, const BBox3D &b);

} // namespace duckdb_3d
