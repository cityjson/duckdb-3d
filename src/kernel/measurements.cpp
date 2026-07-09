#include "kernel/measurements.hpp"
#include <algorithm>
#include <cmath>
#include <map>
#include <stdexcept>
#include <utility>

namespace duckdb_3d {

namespace {

Vertex3D ComputeRingAreaVector(const SolidModel &model, uint32_t ring_idx) {
	uint32_t vi_start = model.ring_vertex_offsets[ring_idx];
	uint32_t vi_end = model.ring_vertex_offsets[ring_idx + 1];
	uint32_t n = vi_end - vi_start;

	Vertex3D area = {0, 0, 0};
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

double SignedTriangleVolume(const SolidModel &model, uint32_t triangle_idx) {
	uint32_t i0 = model.triangle_vertex_indices[triangle_idx * 3 + 0];
	uint32_t i1 = model.triangle_vertex_indices[triangle_idx * 3 + 1];
	uint32_t i2 = model.triangle_vertex_indices[triangle_idx * 3 + 2];

	const auto &a = model.vertices[i0];
	const auto &b = model.vertices[i1];
	const auto &c = model.vertices[i2];

	double cross_x = b.y * c.z - b.z * c.y;
	double cross_y = b.z * c.x - b.x * c.z;
	double cross_z = b.x * c.y - b.y * c.x;
	return a.x * cross_x + a.y * cross_y + a.z * cross_z;
}

} // namespace

double ComputeSurfaceArea(const SolidModel &model) {
	if (model.validation.degenerate_face_count > 0) {
		throw std::runtime_error("ST_3DSurfaceArea: solid contains degenerate faces");
	}
	if (model.TriangleCount() == 0) {
		throw std::runtime_error("ST_3DSurfaceArea: solid has no triangulation cache");
	}

	double total_area = 0.0;
	uint32_t face_count = model.FaceCount();

	for (uint32_t face_idx = 0; face_idx < face_count; face_idx++) {
		uint32_t ring_start = model.face_ring_offsets[face_idx];
		uint32_t ring_end = model.face_ring_offsets[face_idx + 1];
		Vertex3D face_area = {0, 0, 0};

		for (uint32_t ring_idx = ring_start; ring_idx < ring_end; ring_idx++) {
			auto ring_area = ComputeRingAreaVector(model, ring_idx);
			face_area.x += ring_area.x;
			face_area.y += ring_area.y;
			face_area.z += ring_area.z;
		}

		total_area +=
		    0.5 * std::sqrt(face_area.x * face_area.x + face_area.y * face_area.y + face_area.z * face_area.z);
	}

	return total_area;
}

double ComputeVolume(const SolidModel &model) {
	if (!model.validation.is_closed) {
		throw std::runtime_error("ST_3DVolume: solid is not closed");
	}
	if (!model.validation.is_manifold) {
		throw std::runtime_error("ST_3DVolume: solid is not manifold");
	}
	if (!model.validation.is_oriented) {
		throw std::runtime_error("ST_3DVolume: solid has inconsistent orientation");
	}
	if (model.validation.degenerate_face_count > 0) {
		throw std::runtime_error("ST_3DVolume: solid contains degenerate faces");
	}
	if (model.TriangleCount() == 0) {
		throw std::runtime_error("ST_3DVolume: solid has no triangulation cache");
	}

	double total_volume = 0.0;
	uint32_t solid_count = model.SolidCount();

	for (uint32_t solid_idx = 0; solid_idx < solid_count; solid_idx++) {
		double solid_volume = 0.0;
		uint32_t shell_start = model.solid_shell_offsets[solid_idx];
		uint32_t shell_end = model.solid_shell_offsets[solid_idx + 1];

		for (uint32_t shell_idx = shell_start; shell_idx < shell_end; shell_idx++) {
			uint32_t face_start = model.shell_face_offsets[shell_idx];
			uint32_t face_end = model.shell_face_offsets[shell_idx + 1];

			for (uint32_t face_idx = face_start; face_idx < face_end; face_idx++) {
				uint32_t tri_start = model.face_triangle_offsets[face_idx];
				uint32_t tri_end = model.face_triangle_offsets[face_idx + 1];
				for (uint32_t tri_idx = tri_start; tri_idx < tri_end; tri_idx++) {
					solid_volume += SignedTriangleVolume(model, tri_idx);
				}
			}
		}

		total_volume += std::abs(solid_volume);
	}

	return total_volume / 6.0;
}

double ComputeFootprintArea(const SolidModel &model) {
	// For each face, the z-component of its summed ring area vectors equals
	// twice the signed area of the face's XY projection. Up-facing and
	// down-facing faces each project onto the footprint exactly once, so the
	// sum of absolute projected areas is twice the footprint.
	double total_projected = 0.0;
	uint32_t face_count = model.FaceCount();

	for (uint32_t face_idx = 0; face_idx < face_count; face_idx++) {
		uint32_t ring_start = model.face_ring_offsets[face_idx];
		uint32_t ring_end = model.face_ring_offsets[face_idx + 1];
		double face_area_z = 0.0;

		for (uint32_t ring_idx = ring_start; ring_idx < ring_end; ring_idx++) {
			face_area_z += ComputeRingAreaVector(model, ring_idx).z;
		}

		total_projected += std::abs(face_area_z) * 0.5;
	}

	return total_projected * 0.5;
}

double ComputePerimeter(const SolidModel &model) {
	// Count how many faces reference each undirected edge. Boundary edges are
	// those used exactly once; their total length is the perimeter.
	std::map<std::pair<uint32_t, uint32_t>, uint32_t> edge_use;
	uint32_t ring_count = model.RingCount();

	for (uint32_t ring_idx = 0; ring_idx < ring_count; ring_idx++) {
		uint32_t vi_start = model.ring_vertex_offsets[ring_idx];
		uint32_t vi_end = model.ring_vertex_offsets[ring_idx + 1];
		uint32_t n = vi_end - vi_start;
		for (uint32_t i = 0; i < n; i++) {
			uint32_t a = model.ring_vertex_indices[vi_start + i];
			uint32_t b = model.ring_vertex_indices[vi_start + ((i + 1) % n)];
			auto key = std::minmax(a, b);
			edge_use[{key.first, key.second}]++;
		}
	}

	double perimeter = 0.0;
	for (const auto &entry : edge_use) {
		if (entry.second == 1) {
			const auto &va = model.vertices[entry.first.first];
			const auto &vb = model.vertices[entry.first.second];
			double dx = va.x - vb.x, dy = va.y - vb.y, dz = va.z - vb.z;
			perimeter += std::sqrt(dx * dx + dy * dy + dz * dz);
		}
	}

	return perimeter;
}

} // namespace duckdb_3d
