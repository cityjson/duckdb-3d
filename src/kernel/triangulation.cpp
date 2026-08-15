#include "kernel/triangulation.hpp"
#include "kernel/geometry_math.hpp"
#include <cmath>
#include <cstddef>
#include <vector>
#include <algorithm>

namespace duckdb_3d {

namespace {

//! Compute face normal using Newell's method
Vertex3D ComputeFaceNormal(const SolidModel &model, uint32_t face_idx) {
	uint32_t ring_start = model.face_ring_offsets[face_idx];
	double nx = 0, ny = 0, nz = 0;
	uint32_t ring_end = model.face_ring_offsets[face_idx + 1];

	for (uint32_t ring_idx = ring_start; ring_idx < ring_end; ring_idx++) {
		auto ring_normal = NewellRingAreaVector(model, ring_idx);
		nx += ring_normal.x;
		ny += ring_normal.y;
		nz += ring_normal.z;
	}

	double len = std::sqrt(nx * nx + ny * ny + nz * nz);
	if (len < EPSILON) {
		return {0, 0, 1}; // fallback for degenerate faces
	}
	return {nx / len, ny / len, nz / len};
}

//! Project a 3D point to 2D given a face normal.
//! Chooses the two axes that maximize projection quality.
struct Point2D {
	double x, y;
};

void ProjectTo2D(const Vertex3D &v, const Vertex3D &normal, double &out_x, double &out_y) {
	// Drop the axis aligned with the largest normal component
	double ax = std::abs(normal.x), ay = std::abs(normal.y), az = std::abs(normal.z);
	if (az >= ax && az >= ay) {
		out_x = v.x;
		out_y = v.y;
	} else if (ay >= ax) {
		out_x = v.x;
		out_y = v.z;
	} else {
		out_x = v.y;
		out_y = v.z;
	}
}

//! Compute signed area of 2D triangle
double SignedArea2D(double ax, double ay, double bx, double by, double cx, double cy) {
	return 0.5 * ((bx - ax) * (cy - ay) - (cx - ax) * (by - ay));
}

//! Simple ear-clipping triangulation for a convex or simple polygon.
//! Takes ring vertex indices, projects to 2D, and outputs triangle indices.
void EarClipTriangulate(const SolidModel &model, const Vertex3D &normal, uint32_t vi_start, uint32_t vi_end,
                        std::vector<uint32_t> &out_triangles) {
	uint32_t n = vi_end - vi_start;
	if (n < 3) {
		return;
	}

	// Build working list of vertex indices
	std::vector<uint32_t> indices(n);
	for (uint32_t i = 0; i < n; i++) {
		indices[i] = model.ring_vertex_indices[vi_start + i];
	}

	// Project all vertices to 2D
	std::vector<double> px(n), py(n);
	for (uint32_t i = 0; i < n; i++) {
		ProjectTo2D(model.vertices[indices[i]], normal, px[i], py[i]);
	}

	// Ensure CCW winding in 2D
	double total_area = 0;
	for (uint32_t i = 0; i < n; i++) {
		uint32_t j = (i + 1) % n;
		total_area += px[i] * py[j] - px[j] * py[i];
	}
	bool ccw = (total_area > 0);

	// Simple ear-clipping
	std::vector<uint32_t> remaining(n);
	for (uint32_t i = 0; i < n; i++) {
		remaining[i] = i;
	}

	int max_iter = static_cast<int>(n) * static_cast<int>(n);
	int iter = 0;

	while (remaining.size() > 2 && iter < max_iter) {
		bool found_ear = false;
		size_t rn = remaining.size();

		for (size_t i = 0; i < rn; i++) {
			size_t prev = (i + rn - 1) % rn;
			size_t next = (i + 1) % rn;

			uint32_t pi = remaining[prev];
			uint32_t ci = remaining[i];
			uint32_t ni = remaining[next];

			double area = SignedArea2D(px[pi], py[pi], px[ci], py[ci], px[ni], py[ni]);

			// Check correct winding
			bool convex = ccw ? (area > 0) : (area < 0);
			if (!convex) {
				iter++;
				continue;
			}

			// Check no other vertex inside this triangle
			bool has_point_inside = false;
			for (size_t j = 0; j < rn; j++) {
				if (j == prev || j == i || j == next) {
					continue;
				}
				uint32_t ti = remaining[j];
				double a1 = SignedArea2D(px[pi], py[pi], px[ci], py[ci], px[ti], py[ti]);
				double a2 = SignedArea2D(px[ci], py[ci], px[ni], py[ni], px[ti], py[ti]);
				double a3 = SignedArea2D(px[ni], py[ni], px[pi], py[pi], px[ti], py[ti]);

				bool inside;
				if (ccw) {
					inside = (a1 >= 0 && a2 >= 0 && a3 >= 0);
				} else {
					inside = (a1 <= 0 && a2 <= 0 && a3 <= 0);
				}
				if (inside) {
					has_point_inside = true;
					break;
				}
			}

			if (!has_point_inside) {
				out_triangles.push_back(indices[pi]);
				out_triangles.push_back(indices[ci]);
				out_triangles.push_back(indices[ni]);
				remaining.erase(remaining.begin() + static_cast<ptrdiff_t>(i));
				found_ear = true;
				break;
			}
			iter++;
		}

		if (!found_ear) {
			break;
		}
	}
}

} // anonymous namespace

void TriangulateSolidModel(SolidModel &model) {
	uint32_t face_count = model.FaceCount();
	model.face_triangle_offsets.resize(face_count + 1);
	model.triangle_vertex_indices.clear();

	uint32_t tri_offset = 0;

	for (uint32_t f = 0; f < face_count; f++) {
		model.face_triangle_offsets[f] = tri_offset;

		auto normal = ComputeFaceNormal(model, f);

		uint32_t ring_start = model.face_ring_offsets[f];
		uint32_t ring_end = model.face_ring_offsets[f + 1];
		for (uint32_t ring_idx = ring_start; ring_idx < ring_end; ring_idx++) {
			uint32_t vi_start = model.ring_vertex_offsets[ring_idx];
			uint32_t vi_end = model.ring_vertex_offsets[ring_idx + 1];
			EarClipTriangulate(model, normal, vi_start, vi_end, model.triangle_vertex_indices);
		}

		uint32_t new_tris = static_cast<uint32_t>(model.triangle_vertex_indices.size() / 3) - tri_offset;
		tri_offset += new_tris;
	}

	model.face_triangle_offsets[face_count] = tri_offset;
}

} // namespace duckdb_3d
