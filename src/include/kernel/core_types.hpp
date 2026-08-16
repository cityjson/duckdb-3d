#pragma once

#include <cstdint>

namespace duckdb_3d {

//! Absolute tolerance for near-zero floating-point comparisons (vertex dedup,
//! degenerate determinants/normal lengths). Formerly solid_model.hpp EPSILON.
constexpr double kEpsAbsolute = 1e-12;
//! Relative, scale-aware tolerance (ring planarity, shell-volume degeneracy).
constexpr double kEpsRelative = 1e-9;
//! Absolute tolerance below which a signed 2D polygon area is treated as zero.
constexpr double kEpsArea = 1e-18;
//! Distance below which two geometries count as touching (st_3dintersects).
constexpr double kEpsIntersect = 1e-9;

struct Vertex3D {
	double x;
	double y;
	double z;

	bool operator==(const Vertex3D &other) const {
		return x == other.x && y == other.y && z == other.z;
	}
};

struct BBox3D {
	double min_x, min_y, min_z;
	double max_x, max_y, max_z;
};

} // namespace duckdb_3d
