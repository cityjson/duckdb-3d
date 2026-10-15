#include "kernel/geom_model.hpp"

#include <limits>

namespace duckdb_3d {

void GeomModel::ComputeBBox() {
	if (vertices.empty()) {
		bbox = {};
		return;
	}
	double lo = std::numeric_limits<double>::infinity();
	double hi = -std::numeric_limits<double>::infinity();
	bbox = {lo, lo, lo, hi, hi, hi};
	for (const auto &v : vertices) {
		if (v.x < bbox.min_x) {
			bbox.min_x = v.x;
		}
		if (v.y < bbox.min_y) {
			bbox.min_y = v.y;
		}
		if (v.z < bbox.min_z) {
			bbox.min_z = v.z;
		}
		if (v.x > bbox.max_x) {
			bbox.max_x = v.x;
		}
		if (v.y > bbox.max_y) {
			bbox.max_y = v.y;
		}
		if (v.z > bbox.max_z) {
			bbox.max_z = v.z;
		}
	}
}

uint32_t GeomModel::PolygonCount() const {
	switch (type) {
	case GeomType::Polygon:
		return ring_offsets.size() >= 2 ? 1 : 0;
	case GeomType::MultiPolygon:
	case GeomType::PolyhedralSurface:
		return part_offsets.empty() ? 0 : static_cast<uint32_t>(part_offsets.size() - 1);
	default:
		return 0;
	}
}

void GeomModel::PolygonRings(uint32_t polygon, uint32_t &first_ring, uint32_t &ring_count) const {
	if (type == GeomType::Polygon) {
		first_ring = 0;
		ring_count = static_cast<uint32_t>(ring_offsets.size() - 1);
		return;
	}
	first_ring = part_offsets[polygon];
	ring_count = part_offsets[polygon + 1] - part_offsets[polygon];
}

} // namespace duckdb_3d
