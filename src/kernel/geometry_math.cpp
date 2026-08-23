#include "kernel/geometry_math.hpp"

namespace duckdb_3d {

Vertex3D NewellRingAreaVector(const SolidModel &model, uint32_t ring_idx) {
	uint32_t vi_start = model.ring_vertex_offsets[ring_idx];
	uint32_t vi_end = model.ring_vertex_offsets[ring_idx + 1];
	uint32_t n = vi_end - vi_start;

	Vertex3D area = {0, 0, 0};
	if (n < 3) {
		return area;
	}

	// Reference every vertex to the ring's first one. Newell's area vector is
	// translation-invariant, so this is exact — but it is not optional. The mixed
	// form pairs a coordinate DIFFERENCE with a coordinate SUM, so on absolute
	// coordinates the products scale as |position| while the answer scales as
	// |extent|^2, leaving noise of order n·eps·|position|·|extent| once the terms
	// cancel. That noise is what validation.cpp's degeneracy test compares against
	// kEpsAbsolute (1e-12): at RD New (EPSG:28992) magnitudes it reaches 1e-11 to
	// 1e-10, so a face with exactly zero area passes the test that correctly
	// rejects it near the origin — and degenerate_face_count gates ComputeVolume
	// and ComputeSurfaceArea. Referenced locally the same ring returns 0 exactly,
	// at any magnitude.
	const auto &origin = model.vertices[model.ring_vertex_indices[vi_start]];

	for (uint32_t i = 0; i < n; i++) {
		uint32_t idx_cur = model.ring_vertex_indices[vi_start + i];
		uint32_t idx_next = model.ring_vertex_indices[vi_start + ((i + 1) % n)];
		const auto &vcur = model.vertices[idx_cur];
		const auto &vnext = model.vertices[idx_next];
		const Vertex3D cur = {vcur.x - origin.x, vcur.y - origin.y, vcur.z - origin.z};
		const Vertex3D next = {vnext.x - origin.x, vnext.y - origin.y, vnext.z - origin.z};
		area.x += (cur.y - next.y) * (cur.z + next.z);
		area.y += (cur.z - next.z) * (cur.x + next.x);
		area.z += (cur.x - next.x) * (cur.y + next.y);
	}

	return area;
}

bool ShellLocalOrigin(const SolidModel &model, uint32_t shell_idx, Vertex3D &out) {
	uint32_t face_start = model.shell_face_offsets[shell_idx];
	uint32_t face_end = model.shell_face_offsets[shell_idx + 1];

	for (uint32_t f = face_start; f < face_end; f++) {
		uint32_t tri_start = model.face_triangle_offsets[f];
		uint32_t tri_end = model.face_triangle_offsets[f + 1];
		if (tri_start < tri_end) {
			out = model.vertices[model.triangle_vertex_indices[tri_start * 3 + 0]];
			return true;
		}
	}
	return false;
}

} // namespace duckdb_3d
