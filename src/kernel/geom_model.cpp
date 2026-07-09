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

} // namespace duckdb_3d
