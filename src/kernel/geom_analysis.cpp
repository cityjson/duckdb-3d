#include "kernel/geom_analysis.hpp"

#include <algorithm>
#include <cmath>

namespace duckdb_3d {

namespace {

//! Whether the vertices [begin,end) of `m` are coplanar within tolerance.
bool RingIsPlanar(const GeomModel &m, uint32_t begin, uint32_t end) {
	uint32_t n = end - begin;
	if (n <= 3) {
		return true; // three or fewer points are always coplanar
	}
	// Newell's robust polygon normal and the vertex centroid.
	double nx = 0, ny = 0, nz = 0;
	double cx = 0, cy = 0, cz = 0;
	double scale = 1.0;
	for (uint32_t i = begin; i < end; i++) {
		const auto &a = m.vertices[i];
		const auto &b = m.vertices[(i + 1 < end) ? i + 1 : begin];
		nx += (a.y - b.y) * (a.z + b.z);
		ny += (a.z - b.z) * (a.x + b.x);
		nz += (a.x - b.x) * (a.y + b.y);
		cx += a.x;
		cy += a.y;
		cz += a.z;
		scale = std::max({scale, std::fabs(a.x), std::fabs(a.y), std::fabs(a.z)});
	}
	double len = std::sqrt(nx * nx + ny * ny + nz * nz);
	if (len < 1e-12) {
		return true; // degenerate / collinear → treat as planar (a line)
	}
	nx /= len;
	ny /= len;
	nz /= len;
	cx /= n;
	cy /= n;
	cz /= n;
	double tol = 1e-9 * scale;
	for (uint32_t i = begin; i < end; i++) {
		const auto &v = m.vertices[i];
		double d = (v.x - cx) * nx + (v.y - cy) * ny + (v.z - cz) * nz;
		if (std::fabs(d) > tol) {
			return false;
		}
	}
	return true;
}

} // namespace

bool Geom3DIsPlanar(const GeomModel &geom) {
	switch (geom.type) {
	case GeomType::Polygon:
	case GeomType::MultiPolygon:
	case GeomType::PolyhedralSurface:
		// Every ring (each face's rings) must be planar.
		for (size_t r = 0; r + 1 < geom.ring_offsets.size(); r++) {
			if (!RingIsPlanar(geom, geom.ring_offsets[r], geom.ring_offsets[r + 1])) {
				return false;
			}
		}
		return true;
	default:
		// Points and (multi)lines: the whole vertex list as one set.
		return RingIsPlanar(geom, 0, static_cast<uint32_t>(geom.vertices.size()));
	}
}

Vertex3D Geom3DCentroid(const GeomModel &geom) {
	return Vertex3D{0, 0, 0}; // stub
}

} // namespace duckdb_3d
