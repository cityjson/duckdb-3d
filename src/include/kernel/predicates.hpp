#pragma once

#include "kernel/core_types.hpp"

namespace duckdb_3d {

//! Exact sign of ((b - a) x (c - a)) . (d - a): +1 when d lies on the side the
//! right-handed normal of a, b, c points to, -1 on the other side, 0 when the
//! four points are coplanar. Shewchuk's adaptive orient3d, so the sign is exact
//! for any double input; the magnitude is not returned.
int Orient3D(const Vertex3D &a, const Vertex3D &b, const Vertex3D &c, const Vertex3D &d);

//! Exact sign of component `axis` (0 x, 1 y, 2 z) of (b - a) x (c - a): the
//! orientation of a, b, c seen from the positive end of that axis (+1
//! counterclockwise, -1 clockwise, 0 collinear in that projection).
int Orient2D(const Vertex3D &a, const Vertex3D &b, const Vertex3D &c, int axis);

//! Whether a, b, c are exactly collinear (coincident points included): every
//! component of (b - a) x (c - a) is exactly zero.
bool Collinear(const Vertex3D &a, const Vertex3D &b, const Vertex3D &c);

} // namespace duckdb_3d
