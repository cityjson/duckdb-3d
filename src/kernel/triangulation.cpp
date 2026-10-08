#include "kernel/triangulation.hpp"
#include "kernel/geometry_math.hpp"

// earcut.hpp reads points through std::tuple_element / std::get, so the point
// type's headers must precede it.
#include <array>
#include <tuple>

#include "mapbox/earcut.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <initializer_list>
#include <vector>

namespace duckdb_3d {

namespace {

using Point2D = std::array<double, 2>;

//! Buffers TriangulateFace needs for every face, kept across the faces of one
//! model so a solid's triangulation allocates per solid rather than per face.
//! Nothing in here carries meaning from one face to the next.
struct TriangulationScratch {
	std::vector<std::vector<Point2D>> polygon; //!< the face's rings, in its plane
	std::vector<uint32_t> flat_to_vertex;      //!< earcut's flattened index -> model vertex
	std::vector<uint32_t> ring_offsets;        //!< each ring's start in the flattened index space
	std::vector<Point2D> flat;                 //!< the rings' points, flattened
	std::vector<bool> used;                    //!< RestoreDroppedVertices: vertex kept by earcut
	std::vector<uint32_t> run;                 //!< RestoreDroppedVertices: a run of dropped vertices
	std::vector<uint32_t> chain;               //!< RestoreDroppedVertices: the fan replacing a triangle
	mapbox::detail::Earcut<uint32_t> earcut;
};

//! Unnormalised face area vector: the sum of the face's ring Newell vectors.
Vertex3D FaceAreaVector(const SolidModel &model, uint32_t face_idx) {
	Vertex3D n = {0, 0, 0};
	for (uint32_t ring_idx = model.face_ring_offsets[face_idx]; ring_idx < model.face_ring_offsets[face_idx + 1];
	     ring_idx++) {
		auto ring_normal = NewellRingAreaVector(model, ring_idx);
		n.x += ring_normal.x;
		n.y += ring_normal.y;
		n.z += ring_normal.z;
	}
	return n;
}

//! An orthonormal frame (u, v) spanning the plane perpendicular to a face's
//! unit normal n, with (u, v, n) right-handed.
struct PlaneFrame {
	Vertex3D u;
	Vertex3D v;
};

PlaneFrame FrameFor(const Vertex3D &n) {
	// Cross n with the coordinate axis it is least aligned with, so u is never
	// close to degenerate.
	double ax = std::abs(n.x), ay = std::abs(n.y), az = std::abs(n.z);
	Vertex3D e = (ax <= ay && ax <= az) ? Vertex3D {1, 0, 0} : (ay <= az ? Vertex3D {0, 1, 0} : Vertex3D {0, 0, 1});
	Vertex3D u = {n.y * e.z - n.z * e.y, n.z * e.x - n.x * e.z, n.x * e.y - n.y * e.x};
	double len = std::hypot(std::hypot(u.x, u.y), u.z);
	u = {u.x / len, u.y / len, u.z / len};
	Vertex3D v = {n.y * u.z - n.z * u.y, n.z * u.x - n.x * u.z, n.x * u.y - n.y * u.x};
	return {u, v};
}

//! Coordinates of `p` in the face plane, relative to `origin`.
//!
//! Projecting onto the plane itself, rather than dropping the coordinate axis the
//! normal leans on most, keeps distinct vertices distinct: on a near-vertical
//! sliver whose normal sits between two axes, dropping an axis can land two
//! vertices on one 2D point, and the triangulation would silently lose one.
Point2D ToPlane(const Vertex3D &p, const Vertex3D &origin, const PlaneFrame &frame) {
	double dx = p.x - origin.x, dy = p.y - origin.y, dz = p.z - origin.z;
	return {dx * frame.u.x + dy * frame.u.y + dz * frame.u.z, dx * frame.v.x + dy * frame.v.y + dz * frame.v.z};
}

//! True when every value is finite (neither infinite nor NaN).
bool AllFinite(std::initializer_list<double> values) {
	for (double v : values) {
		if (!std::isfinite(v)) {
			return false;
		}
	}
	return true;
}

//! Twice the signed area of a 2D ring (shoelace).
double SignedArea2(const std::vector<Point2D> &ring) {
	double sum = 0;
	for (size_t i = 0, n = ring.size(); i < n; i++) {
		const auto &a = ring[i];
		const auto &b = ring[(i + 1) % n];
		sum += a[0] * b[1] - b[0] * a[1];
	}
	return sum;
}

//! Twice the signed area of a 2D triangle.
double SignedArea2(const Point2D &a, const Point2D &b, const Point2D &c) {
	return (b[0] - a[0]) * (c[1] - a[1]) - (c[0] - a[0]) * (b[1] - a[1]);
}

//! Put back every ring vertex earcut left out of the triangles.
//!
//! earcut drops a vertex that is collinear with its neighbours in 2D, or that
//! coincides with one. That is harmless for a planar face, but a face that is
//! only nearly planar can have a vertex that is collinear in the face plane yet
//! off it in 3D, and the neighbouring face still uses that vertex: skipping it
//! leaves a sliver gap between the two faces' triangles, so the triangulated
//! surface is no longer closed and the signed-volume sum is off by the sliver.
//!
//! Each run of dropped vertices on a ring lies along the boundary edge (p, q)
//! joining the kept vertices around it, and that edge belongs to exactly one
//! triangle (p, q, c). Fanning that triangle from c through the run restores
//! every vertex without changing the covered 2D area. `ring_offsets` are the
//! rings' start positions in earcut's flattened index space, plus the end.
//! Returns false when a run's boundary edge is not in the triangulation, which
//! means the triangles do not follow the ring and cannot be repaired here.
bool RestoreDroppedVertices(std::vector<uint32_t> &indices, const std::vector<uint32_t> &ring_offsets,
                            TriangulationScratch &scratch) {
	uint32_t total = ring_offsets.back();
	auto &used = scratch.used;
	used.assign(total, false);
	for (auto i : indices) {
		used[i] = true;
	}
	for (size_t r = 0; r + 1 < ring_offsets.size(); r++) {
		uint32_t start = ring_offsets[r];
		uint32_t n = ring_offsets[r + 1] - start;
		uint32_t first_kept = n;
		for (uint32_t k = 0; k < n; k++) {
			if (used[start + k]) {
				first_kept = k;
				break;
			}
		}
		if (first_kept == n) {
			return false; // a whole ring dropped: the triangles do not cover it
		}
		// Walk the ring once from a kept vertex, collecting each run of dropped
		// vertices between two kept ones.
		uint32_t p = start + first_kept;
		auto &run = scratch.run;
		run.clear();
		for (uint32_t step = 1; step <= n; step++) {
			uint32_t cur = start + (first_kept + step) % n;
			if (!used[cur]) {
				run.push_back(cur);
				continue;
			}
			if (!run.empty()) {
				// Find the triangle holding boundary edge {p, cur}.
				size_t tri = indices.size();
				bool forward = false;
				for (size_t t = 0; t + 2 < indices.size() && tri == indices.size(); t += 3) {
					for (int e = 0; e < 3; e++) {
						uint32_t a = indices[t + e], b = indices[t + (e + 1) % 3];
						if (a == p && b == cur) {
							tri = t + e;
							forward = true;
						} else if (a == cur && b == p) {
							tri = t + e;
							forward = false;
						}
						if (tri != indices.size()) {
							break;
						}
					}
				}
				if (tri == indices.size()) {
					return false;
				}
				size_t base = tri - tri % 3;
				uint32_t a = indices[tri];
				uint32_t c = indices[base + (tri - base + 2) % 3];
				uint32_t b = indices[base + (tri - base + 1) % 3];
				// Triangle (a, b, c) keeps its winding: replace it by the fan
				// a -> run... -> b, each closed by c.
				auto &chain = scratch.chain;
				chain.clear();
				chain.push_back(a);
				if (forward) {
					chain.insert(chain.end(), run.begin(), run.end());
				} else {
					chain.insert(chain.end(), run.rbegin(), run.rend());
				}
				chain.push_back(b);
				indices[base] = chain[0];
				indices[base + 1] = chain[1];
				indices[base + 2] = c;
				for (size_t k = 1; k + 1 < chain.size(); k++) {
					indices.push_back(chain[k]);
					indices.push_back(chain[k + 1]);
					indices.push_back(c);
				}
				for (auto v : run) {
					used[v] = true;
				}
				run.clear();
			}
			p = cur;
		}
	}
	return true;
}

//! Triangulate one face — its exterior ring and any holes together, the holes
//! bridged into the exterior — and append the triangles to `out`, wound like the
//! exterior ring. Returns false, appending nothing, when the triangles would not
//! tile the face: a ring that self-intersects or a hole that leaves the exterior
//! has no triangulation whose area matches the polygon's, and handing back a
//! partial one would silently corrupt every measurement that sums triangles.
bool TriangulateFace(const SolidModel &model, uint32_t face_idx, std::vector<uint32_t> &out,
                     TriangulationScratch &scratch) {
	uint32_t ring_start = model.face_ring_offsets[face_idx];
	uint32_t ring_end = model.face_ring_offsets[face_idx + 1];
	if (ring_start == ring_end) {
		return false;
	}

	// Every quantity from here to earcut must be finite. A face whose area
	// overflows the double range (coordinates near 1e154 and beyond) has a
	// non-finite normal, hence a NaN frame and NaN 2D coordinates; earcut's
	// z-order hash would cast those to int32 (undefined behaviour), and every
	// comparison in the tiling check is false for NaN. Such a face is rejected
	// here, and validation reports it degenerate.
	auto normal = FaceAreaVector(model, face_idx);
	if (!std::isfinite(normal.x) || !std::isfinite(normal.y) || !std::isfinite(normal.z)) {
		return false;
	}
	double normal_len = std::hypot(std::hypot(normal.x, normal.y), normal.z); // no intermediate overflow
	if (!std::isfinite(normal_len)) {
		return false;
	}
	if (normal_len < kEpsAbsolute) {
		return false; // zero-area face: nothing to tile
	}
	auto frame = FrameFor({normal.x / normal_len, normal.y / normal_len, normal.z / normal_len});
	if (!AllFinite({frame.u.x, frame.u.y, frame.u.z, frame.v.x, frame.v.y, frame.v.z})) {
		return false;
	}

	// Project every ring into the face plane, referenced to the exterior ring's
	// first vertex.
	//
	// The local origin is not cosmetic. Every quantity below is a signed area, a
	// difference of products of coordinates; on absolute projected coordinates
	// those products scale as |position|^2 while the answer scales as |extent|^2,
	// so the small ones drown (DESIGN_DOC §8.2 makes the same point about volume,
	// one power higher). Shifting is exact for areas, which are
	// translation-invariant, and keeps the products at face scale.
	const auto &origin = model.vertices[model.ring_vertex_indices[model.ring_vertex_offsets[ring_start]]];
	auto &polygon = scratch.polygon;
	auto &flat_to_vertex = scratch.flat_to_vertex;
	auto &ring_offsets = scratch.ring_offsets;
	polygon.resize(ring_end - ring_start);
	flat_to_vertex.clear();
	ring_offsets.clear();
	double min_x = 0, max_x = 0, min_y = 0, max_y = 0;
	for (uint32_t ring_idx = ring_start; ring_idx < ring_end; ring_idx++) {
		uint32_t vi_start = model.ring_vertex_offsets[ring_idx];
		uint32_t vi_end = model.ring_vertex_offsets[ring_idx + 1];
		if (vi_end - vi_start < 3) {
			return false;
		}
		ring_offsets.push_back(static_cast<uint32_t>(flat_to_vertex.size()));
		auto &ring = polygon[ring_idx - ring_start];
		ring.clear();
		for (uint32_t vi = vi_start; vi < vi_end; vi++) {
			uint32_t vertex = model.ring_vertex_indices[vi];
			auto p = ToPlane(model.vertices[vertex], origin, frame);
			if (!std::isfinite(p[0]) || !std::isfinite(p[1])) {
				return false;
			}
			min_x = std::min(min_x, p[0]);
			max_x = std::max(max_x, p[0]);
			min_y = std::min(min_y, p[1]);
			max_y = std::max(max_y, p[1]);
			ring.push_back(p);
			flat_to_vertex.push_back(vertex);
		}
	}
	ring_offsets.push_back(static_cast<uint32_t>(flat_to_vertex.size()));

	// The area the triangles must cover: the exterior less its holes, whichever
	// way each ring happens to be wound.
	double exterior2 = SignedArea2(polygon[0]);
	double expected2 = std::abs(exterior2);
	for (size_t h = 1; h < polygon.size(); h++) {
		expected2 -= std::abs(SignedArea2(polygon[h]));
	}

	// The face's 2D extent and area feed the tiling tolerance; they too must be
	// representable.
	if (!AllFinite({(max_x - min_x) * (max_y - min_y), expected2})) {
		return false;
	}

	// earcut's fallback splits nest, and the vendored copy caps the nesting so a
	// self-overlapping ring cannot exhaust the stack (third_party/README.md).
	// Hitting the cap leaves the indices unusable: reject the face.
	auto &earcut = scratch.earcut;
	earcut(polygon);
	if (earcut.splitDepthExceeded) {
		return false;
	}
	auto &indices = earcut.indices;

	// Accept the result only if it tiles the face: every triangle faces one way,
	// and together they cover exactly the expected area. Rounding is relative to
	// the face's 2D extent, the scale of the products the areas are built from.
	auto &flat = scratch.flat;
	flat.clear();
	for (const auto &ring : polygon) {
		flat.insert(flat.end(), ring.begin(), ring.end());
	}
	double signed2 = 0, abs2 = 0;
	for (size_t t = 0; t + 2 < indices.size(); t += 3) {
		double a = SignedArea2(flat[indices[t]], flat[indices[t + 1]], flat[indices[t + 2]]);
		signed2 += a;
		abs2 += std::abs(a);
	}
	double tolerance2 = kEpsRelative * std::max((max_x - min_x) * (max_y - min_y), expected2);
	if (indices.empty() || expected2 <= 0 || std::abs(abs2 - expected2) > tolerance2 ||
	    std::abs(abs2 - std::abs(signed2)) > tolerance2) {
		return false;
	}
	if (!RestoreDroppedVertices(indices, ring_offsets, scratch)) {
		return false;
	}

	// Wind every triangle like the exterior ring, so a triangle's orientation in
	// 3D follows the face's and signed volume sums stay meaningful.
	bool flip = (signed2 > 0) != (exterior2 > 0);
	for (size_t t = 0; t + 2 < indices.size(); t += 3) {
		out.push_back(flat_to_vertex[indices[t]]);
		out.push_back(flat_to_vertex[indices[flip ? t + 2 : t + 1]]);
		out.push_back(flat_to_vertex[indices[flip ? t + 1 : t + 2]]);
	}
	return true;
}

} // anonymous namespace

void TriangulateSolidModel(SolidModel &model) {
	uint32_t face_count = model.FaceCount();
	model.face_triangle_offsets.resize(face_count + 1);
	model.triangle_vertex_indices.clear();

	TriangulationScratch scratch;
	for (uint32_t f = 0; f < face_count; f++) {
		model.face_triangle_offsets[f] = static_cast<uint32_t>(model.triangle_vertex_indices.size() / 3);
		// A face that cannot be tiled keeps an empty triangle range; validation
		// counts it degenerate (DESIGN_DOC §8.1), which gates volume and area.
		TriangulateFace(model, f, model.triangle_vertex_indices, scratch);
	}
	model.face_triangle_offsets[face_count] = static_cast<uint32_t>(model.triangle_vertex_indices.size() / 3);
}

} // namespace duckdb_3d
