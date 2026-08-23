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

	for (uint32_t i = 0; i < n; i++) {
		uint32_t idx_cur = model.ring_vertex_indices[vi_start + i];
		uint32_t idx_next = model.ring_vertex_indices[vi_start + ((i + 1) % n)];
		const auto &cur = model.vertices[idx_cur];
		const auto &next = model.vertices[idx_next];
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
