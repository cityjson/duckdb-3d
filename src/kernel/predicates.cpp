#include "kernel/predicates.hpp"

#include "predicates/predicates.h"

namespace duckdb_3d {

namespace {

//! exactinit() computes the machine epsilon and splitter the predicates use. A
//! function-local static runs it once, thread-safely, before the first call.
void EnsureExactInit() {
	static const bool initialised = [] {
		three_d_exactinit();
		return true;
	}();
	(void)initialised;
}

int Sign(double value) {
	return (value > 0.0) - (value < 0.0);
}

double Coord(const Vertex3D &v, int axis) {
	return axis == 0 ? v.x : (axis == 1 ? v.y : v.z);
}

} // namespace

int Orient3D(const Vertex3D &a, const Vertex3D &b, const Vertex3D &c, const Vertex3D &d) {
	EnsureExactInit();
	// Shewchuk's orient3d is positive when d lies below the plane through a, b, c
	// seen with a, b, c counterclockwise: the opposite of the normal convention.
	const double pa[3] = {a.x, a.y, a.z};
	const double pb[3] = {b.x, b.y, b.z};
	const double pc[3] = {c.x, c.y, c.z};
	const double pd[3] = {d.x, d.y, d.z};
	return -Sign(three_d_orient3d(pa, pb, pc, pd));
}

int Orient2D(const Vertex3D &a, const Vertex3D &b, const Vertex3D &c, int axis) {
	EnsureExactInit();
	// Cyclic projections (y, z), (z, x), (x, y), so the 2D orientation is the sign
	// of the dropped axis's normal component.
	const int u = (axis + 1) % 3;
	const int v = (axis + 2) % 3;
	const double qa[2] = {Coord(a, u), Coord(a, v)};
	const double qb[2] = {Coord(b, u), Coord(b, v)};
	const double qc[2] = {Coord(c, u), Coord(c, v)};
	return Sign(three_d_orient2d(qa, qb, qc));
}

bool Collinear(const Vertex3D &a, const Vertex3D &b, const Vertex3D &c) {
	return Orient2D(a, b, c, 0) == 0 && Orient2D(a, b, c, 1) == 0 && Orient2D(a, b, c, 2) == 0;
}

} // namespace duckdb_3d
