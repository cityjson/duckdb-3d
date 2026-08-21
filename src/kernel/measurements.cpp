#include "kernel/measurements.hpp"
#include "kernel/geometry_math.hpp"
#include <algorithm>
#include <cmath>
#include <map>
#include <stdexcept>
#include <utility>

namespace duckdb_3d {

namespace {

//! Six times the signed volume of the tetrahedron (origin, a, b, c), with every
//! vertex taken relative to `origin`.
//!
//! The shift is a no-op in exact arithmetic — a closed shell's signed volume is
//! translation-invariant — but it is what makes the sum usable in doubles. On
//! absolute coordinates the triple product scales as |position|^3 while the
//! answer scales as |extent|^3, so a building in a projected CRS (RD New
//! easting/northing ~1e5) cancels roughly nine of the ~16 available digits, and
//! by 1e8 the result is pure noise. Referencing a point inside the model keeps
//! every term at model scale.
double SignedTriangleVolume(const SolidModel &model, uint32_t triangle_idx, const Vertex3D &origin) {
	uint32_t i0 = model.triangle_vertex_indices[triangle_idx * 3 + 0];
	uint32_t i1 = model.triangle_vertex_indices[triangle_idx * 3 + 1];
	uint32_t i2 = model.triangle_vertex_indices[triangle_idx * 3 + 2];

	const auto &va = model.vertices[i0];
	const auto &vb = model.vertices[i1];
	const auto &vc = model.vertices[i2];

	double ax = va.x - origin.x, ay = va.y - origin.y, az = va.z - origin.z;
	double bx = vb.x - origin.x, by = vb.y - origin.y, bz = vb.z - origin.z;
	double cx = vc.x - origin.x, cy = vc.y - origin.y, cz = vc.z - origin.z;

	double cross_x = by * cz - bz * cy;
	double cross_y = bz * cx - bx * cz;
	double cross_z = bx * cy - by * cx;
	return ax * cross_x + ay * cross_y + az * cross_z;
}

//! Midpoint of the model's axis-aligned bounding box — a reference point at the
//! centre of the coordinate range, so no relative coordinate exceeds half the
//! model's extent.
Vertex3D ModelOrigin(const SolidModel &model) {
	if (model.vertices.empty()) {
		return {0.0, 0.0, 0.0};
	}
	Vertex3D lo = model.vertices[0];
	Vertex3D hi = model.vertices[0];
	for (const auto &v : model.vertices) {
		lo.x = std::min(lo.x, v.x);
		lo.y = std::min(lo.y, v.y);
		lo.z = std::min(lo.z, v.z);
		hi.x = std::max(hi.x, v.x);
		hi.y = std::max(hi.y, v.y);
		hi.z = std::max(hi.z, v.z);
	}
	return {0.5 * (lo.x + hi.x), 0.5 * (lo.y + hi.y), 0.5 * (lo.z + hi.z)};
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
			auto ring_area = NewellRingAreaVector(model, ring_idx);
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
	const Vertex3D origin = ModelOrigin(model);

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
					solid_volume += SignedTriangleVolume(model, tri_idx, origin);
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
			face_area_z += NewellRingAreaVector(model, ring_idx).z;
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
