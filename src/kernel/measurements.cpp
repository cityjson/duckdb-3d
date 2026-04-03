#include "kernel/measurements.hpp"
#include <cmath>
#include <stdexcept>

namespace duckdb_3d {

double ComputeSurfaceArea(const SolidModel &model) {
	if (model.validation.degenerate_face_count > 0) {
		throw std::runtime_error("ST_3DSurfaceArea: solid contains degenerate faces");
	}
	if (model.TriangleCount() == 0) {
		throw std::runtime_error("ST_3DSurfaceArea: solid has no triangulation cache");
	}

	double total_area = 0.0;
	uint32_t tri_count = model.TriangleCount();

	for (uint32_t t = 0; t < tri_count; t++) {
		uint32_t i0 = model.triangle_vertex_indices[t * 3 + 0];
		uint32_t i1 = model.triangle_vertex_indices[t * 3 + 1];
		uint32_t i2 = model.triangle_vertex_indices[t * 3 + 2];

		const auto &a = model.vertices[i0];
		const auto &b = model.vertices[i1];
		const auto &c = model.vertices[i2];

		// Cross product (b-a) x (c-a)
		double ux = b.x - a.x, uy = b.y - a.y, uz = b.z - a.z;
		double vx = c.x - a.x, vy = c.y - a.y, vz = c.z - a.z;
		double cx = uy * vz - uz * vy;
		double cy = uz * vx - ux * vz;
		double cz = ux * vy - uy * vx;

		total_area += 0.5 * std::sqrt(cx * cx + cy * cy + cz * cz);
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

	// Signed volume using divergence theorem:
	// V = (1/6) * sum over triangles of: dot(a, cross(b, c))
	// where a, b, c are the triangle vertices in order
	double total_volume = 0.0;
	uint32_t tri_count = model.TriangleCount();

	for (uint32_t t = 0; t < tri_count; t++) {
		uint32_t i0 = model.triangle_vertex_indices[t * 3 + 0];
		uint32_t i1 = model.triangle_vertex_indices[t * 3 + 1];
		uint32_t i2 = model.triangle_vertex_indices[t * 3 + 2];

		const auto &a = model.vertices[i0];
		const auto &b = model.vertices[i1];
		const auto &c = model.vertices[i2];

		// Signed tetrahedral volume contribution:
		// (1/6) * a . (b x c)
		double cross_x = b.y * c.z - b.z * c.y;
		double cross_y = b.z * c.x - b.x * c.z;
		double cross_z = b.x * c.y - b.y * c.x;

		total_volume += a.x * cross_x + a.y * cross_y + a.z * cross_z;
	}

	return std::abs(total_volume) / 6.0;
}

} // namespace duckdb_3d
