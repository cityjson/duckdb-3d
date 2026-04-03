#include "kernel/solid_model.hpp"
#include <algorithm>
#include <limits>

namespace duckdb_3d {

uint32_t SolidModel::SolidCount() const {
	return solid_shell_offsets.empty() ? 0 : static_cast<uint32_t>(solid_shell_offsets.size() - 1);
}

uint32_t SolidModel::ShellCount() const {
	return shell_face_offsets.empty() ? 0 : static_cast<uint32_t>(shell_face_offsets.size() - 1);
}

uint32_t SolidModel::FaceCount() const {
	return face_ring_offsets.empty() ? 0 : static_cast<uint32_t>(face_ring_offsets.size() - 1);
}

uint32_t SolidModel::RingCount() const {
	return ring_vertex_offsets.empty() ? 0 : static_cast<uint32_t>(ring_vertex_offsets.size() - 1);
}

uint32_t SolidModel::TriangleCount() const {
	return static_cast<uint32_t>(triangle_vertex_indices.size() / 3);
}

void SolidModel::ComputeBBox() {
	if (vertices.empty()) {
		bbox = {0, 0, 0, 0, 0, 0};
		return;
	}
	bbox.min_x = bbox.max_x = vertices[0].x;
	bbox.min_y = bbox.max_y = vertices[0].y;
	bbox.min_z = bbox.max_z = vertices[0].z;
	for (size_t i = 1; i < vertices.size(); i++) {
		bbox.min_x = std::min(bbox.min_x, vertices[i].x);
		bbox.min_y = std::min(bbox.min_y, vertices[i].y);
		bbox.min_z = std::min(bbox.min_z, vertices[i].z);
		bbox.max_x = std::max(bbox.max_x, vertices[i].x);
		bbox.max_y = std::max(bbox.max_y, vertices[i].y);
		bbox.max_z = std::max(bbox.max_z, vertices[i].z);
	}
}

} // namespace duckdb_3d
