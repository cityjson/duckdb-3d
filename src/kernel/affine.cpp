#include "kernel/affine.hpp"

#include <cmath>
#include <stdexcept>
#include <string>

namespace duckdb_3d {

AffineTransform3D AffineIdentity() {
	return {{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0}};
}

AffineTransform3D AffineFromMatrix4x4(const std::vector<double> &row_major) {
	if (row_major.size() != 16) {
		throw std::runtime_error("transformation matrix must have 16 values (a row-major 4x4), got " +
		                         std::to_string(row_major.size()));
	}
	for (double value : row_major) {
		if (!std::isfinite(value)) {
			throw std::runtime_error("transformation matrix values must be finite");
		}
	}
	if (row_major[12] != 0 || row_major[13] != 0 || row_major[14] != 0 || row_major[15] != 1) {
		throw std::runtime_error("transformation matrix must be affine: its last row must be 0, 0, 0, 1");
	}
	AffineTransform3D a;
	for (size_t i = 0; i < 12; i++) {
		a.m[i] = row_major[i];
	}
	return a;
}

AffineTransform3D ThenTranslate(const AffineTransform3D &a, const Vertex3D &t) {
	AffineTransform3D out = a;
	out.m[3] += t.x;
	out.m[7] += t.y;
	out.m[11] += t.z;
	return out;
}

Vertex3D ApplyAffine(const AffineTransform3D &a, const Vertex3D &v) {
	const auto &m = a.m;
	return {m[0] * v.x + m[1] * v.y + m[2] * v.z + m[3], m[4] * v.x + m[5] * v.y + m[6] * v.z + m[7],
	        m[8] * v.x + m[9] * v.y + m[10] * v.z + m[11]};
}

void ApplyAffine(const AffineTransform3D &a, std::vector<Vertex3D> &vertices) {
	for (auto &v : vertices) {
		v = ApplyAffine(a, v);
	}
}

double LinearDeterminant(const AffineTransform3D &a) {
	const auto &m = a.m;
	return m[0] * (m[5] * m[10] - m[6] * m[9]) - m[1] * (m[4] * m[10] - m[6] * m[8]) +
	       m[2] * (m[4] * m[9] - m[5] * m[8]);
}

bool IsSingularLinear(const AffineTransform3D &a) {
	const auto &m = a.m;
	double r0 = std::hypot(std::hypot(m[0], m[1]), m[2]);
	double r1 = std::hypot(std::hypot(m[4], m[5]), m[6]);
	double r2 = std::hypot(std::hypot(m[8], m[9]), m[10]);
	double bound = r0 * r1 * r2; // Hadamard: |det| <= r0 * r1 * r2
	return bound == 0 || std::abs(LinearDeterminant(a)) <= kEpsRelative * bound;
}

} // namespace duckdb_3d
