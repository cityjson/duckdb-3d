#pragma once

#include "kernel/core_types.hpp"

#include <array>
#include <vector>

namespace duckdb_3d {

//! A 3D affine transform: the upper three rows of a 4x4 matrix in row-major
//! order, so a point maps as x' = m[0]x + m[1]y + m[2]z + m[3], and likewise for
//! y' (m[4..7]) and z' (m[8..11]). The implied fourth row is (0, 0, 0, 1).
struct AffineTransform3D {
	std::array<double, 12> m;
};

//! The identity transform.
AffineTransform3D AffineIdentity();

//! Build a transform from a flat, row-major 4x4 matrix of exactly 16 finite
//! values whose last row is (0, 0, 0, 1). Throws std::runtime_error otherwise:
//! a projective last row has no affine meaning, and silently dropping it would
//! place geometry somewhere the matrix does not say.
AffineTransform3D AffineFromMatrix4x4(const std::vector<double> &row_major);

//! `a` followed by a translation by `t`: x' = a(x) + t.
AffineTransform3D ThenTranslate(const AffineTransform3D &a, const Vertex3D &t);

//! Map one point.
Vertex3D ApplyAffine(const AffineTransform3D &a, const Vertex3D &v);

//! Map every point in place.
void ApplyAffine(const AffineTransform3D &a, std::vector<Vertex3D> &vertices);

//! Determinant of the linear (3x3) part. Negative for a transform that mirrors,
//! which reverses the handedness of every face's winding; zero for one that
//! collapses a dimension.
double LinearDeterminant(const AffineTransform3D &a);

} // namespace duckdb_3d
