#include "kernel/validation.hpp"
#include "kernel/geometry_math.hpp"
#include "kernel/self_intersection.hpp"
#include <algorithm>
#include <cmath>
#include <utility>

namespace duckdb_3d {

namespace {

double Magnitude(const Vertex3D &v) {
	return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
}

//! Squared distance from a ring's first vertex to its farthest vertex: the
//! length scale a ring's area is compared against.
double RingExtent2(const SolidModel &model, uint32_t ring_idx) {
	uint32_t vi_start = model.ring_vertex_offsets[ring_idx];
	uint32_t vi_end = model.ring_vertex_offsets[ring_idx + 1];
	const auto &o = model.vertices[model.ring_vertex_indices[vi_start]];
	double extent2 = 0;
	for (uint32_t i = vi_start + 1; i < vi_end; i++) {
		const auto &v = model.vertices[model.ring_vertex_indices[i]];
		double dx = v.x - o.x, dy = v.y - o.y, dz = v.z - o.z;
		extent2 = std::max(extent2, dx * dx + dy * dy + dz * dz);
	}
	return extent2;
}

//! Check if a face is degenerate: fewer than 3 distinct vertices in a ring, an
//! area near zero (the sum of all ring area vectors, to account for holes), or
//! a triangulation that failed. "Near zero" is relative to the ring's own
//! extent (kEpsRelative), so the verdict does not depend on the unit of length.
bool IsFaceDegenerate(const SolidModel &model, uint32_t face_idx) {
	uint32_t ring_start = model.face_ring_offsets[face_idx];
	uint32_t ring_end = model.face_ring_offsets[face_idx + 1];
	if (ring_start == ring_end) {
		return true;
	}

	Vertex3D face_area = {0, 0, 0};
	for (uint32_t ring_idx = ring_start; ring_idx < ring_end; ring_idx++) {
		uint32_t vi_start = model.ring_vertex_offsets[ring_idx];
		uint32_t vi_end = model.ring_vertex_offsets[ring_idx + 1];
		if (vi_end - vi_start < 3) {
			return true;
		}

		auto ring_area = NewellRingAreaVector(model, ring_idx);
		if (!(Magnitude(ring_area) > kEpsRelative * RingExtent2(model, ring_idx))) {
			return true;
		}

		face_area.x += ring_area.x;
		face_area.y += ring_area.y;
		face_area.z += ring_area.z;
	}

	if (!(Magnitude(face_area) > kEpsRelative * RingExtent2(model, ring_start))) {
		return true;
	}

	// A face the triangulator could not tile keeps an empty triangle range
	// rather than a partial one (kernel/triangulation.cpp); it has no usable
	// triangles for volume, so it is degenerate.
	if (model.face_triangle_offsets.size() == model.FaceCount() + 1 &&
	    model.face_triangle_offsets[face_idx + 1] == model.face_triangle_offsets[face_idx]) {
		return true;
	}
	return false;
}

struct ShellValidationResult {
	uint32_t open_edges = 0;
	uint32_t non_manifold_edges = 0;
	uint32_t orientation_errors = 0;
	bool is_closed = true;
	bool is_manifold = true;
	bool is_oriented = true;
};

//! Edge keys for one shell, reused from shell to shell so validation allocates
//! once per solid rather than once per shell.
struct EdgeScratch {
	std::vector<uint64_t> directed;   //!< (from << 32) | to, one per ring edge
	std::vector<uint64_t> undirected; //!< (min << 32) | max, one per ring edge
};

//! Calls `f(run_length)` for each run of equal values in a sorted vector.
template <class F>
void ForEachRun(const std::vector<uint64_t> &sorted, F &&f) {
	for (size_t i = 0, n = sorted.size(); i < n;) {
		size_t j = i + 1;
		while (j < n && sorted[j] == sorted[i]) {
			j++;
		}
		f(j - i);
		i = j;
	}
}

//! Closedness, manifoldness and winding consistency of one shell, from its ring
//! edges. An undirected edge used once is open and one used more than twice is
//! non-manifold; a directed edge used more than once means two faces traverse it
//! the same way, an orientation error. (A directed edge used once with no reverse
//! is used once undirected, so it is open, not an orientation error.) Counting by
//! sorting the edge keys gives the same counts a hash map would.
ShellValidationResult ValidateShellTopology(const SolidModel &model, uint32_t shell_idx, EdgeScratch &edges) {
	ShellValidationResult result;
	edges.directed.clear();
	edges.undirected.clear();

	uint32_t face_start = model.shell_face_offsets[shell_idx];
	uint32_t face_end = model.shell_face_offsets[shell_idx + 1];
	for (uint32_t f = face_start; f < face_end; f++) {
		for (uint32_t ring_idx = model.face_ring_offsets[f]; ring_idx < model.face_ring_offsets[f + 1]; ring_idx++) {
			uint32_t vi_start = model.ring_vertex_offsets[ring_idx];
			uint32_t n = model.ring_vertex_offsets[ring_idx + 1] - vi_start;
			for (uint32_t i = 0; i < n; i++) {
				uint64_t from = model.ring_vertex_indices[vi_start + i];
				uint64_t to = model.ring_vertex_indices[vi_start + ((i + 1) % n)];
				edges.directed.push_back((from << 32) | to);
				edges.undirected.push_back(from < to ? ((from << 32) | to) : ((to << 32) | from));
			}
		}
	}

	std::sort(edges.undirected.begin(), edges.undirected.end());
	ForEachRun(edges.undirected, [&](size_t uses) {
		if (uses < 2) {
			result.open_edges++;
			result.is_closed = false;
		} else if (uses > 2) {
			result.non_manifold_edges++;
			result.is_manifold = false;
		}
	});

	std::sort(edges.directed.begin(), edges.directed.end());
	ForEachRun(edges.directed, [&](size_t uses) {
		if (uses > 1) {
			result.orientation_errors++;
			result.is_oriented = false;
		}
	});

	return result;
}

//! Signed volume of a shell: the divergence-theorem sum of origin-based tetrahedra
//! over the shell's triangulation. The sign encodes winding — an outward-wound
//! shell is positive, an inward-wound (cavity) shell negative. Vertices are
//! translated to the shell's first vertex before summing so the magnitudes stay
//! O(shell size)³ rather than O(distance-from-origin)³; this avoids catastrophic
//! cancellation for projected-CRS coordinates (e.g. EPSG:28992 at ~10^5), where a
//! small cavity's true volume would otherwise drown in rounding noise. Translation
//! does not change a closed shell's signed volume. `abs_sum` (the sum of absolute
//! per-triangle contributions) is returned as a conditioning measure for the
//! relative degeneracy test.
struct ShellSignedVolume {
	double signed_vol = 0.0;
	//! Cube of the shell's bounding-box diagonal — a scale for the degeneracy test.
	//! |signed_vol| that is a negligible fraction of this means the shell encloses
	//! no real volume (flat/collapsed), independent of how concave or finely
	//! tessellated it is (so a valid concave shell is not mistaken for degenerate).
	double scale = 0.0;
	bool has_geometry = false;
};

ShellSignedVolume ComputeShellSignedVolume(const SolidModel &model, uint32_t shell_idx) {
	ShellSignedVolume out;
	uint32_t face_start = model.shell_face_offsets[shell_idx];
	uint32_t face_end = model.shell_face_offsets[shell_idx + 1];

	// Local origin: the shell's first triangulated vertex — the same reference
	// point ComputeVolume uses, so the two agree bit-for-bit.
	Vertex3D o = {0, 0, 0};
	if (!ShellLocalOrigin(model, shell_idx, o)) {
		return out; // no triangles → nothing to integrate
	}
	out.has_geometry = true;

	Vertex3D lo = o, hi = o;
	for (uint32_t f = face_start; f < face_end; f++) {
		uint32_t tri_start = model.face_triangle_offsets[f];
		uint32_t tri_end = model.face_triangle_offsets[f + 1];
		for (uint32_t t = tri_start; t < tri_end; t++) {
			const auto &va = model.vertices[model.triangle_vertex_indices[t * 3 + 0]];
			const auto &vb = model.vertices[model.triangle_vertex_indices[t * 3 + 1]];
			const auto &vc = model.vertices[model.triangle_vertex_indices[t * 3 + 2]];
			for (const auto &v : {va, vb, vc}) {
				lo.x = std::min(lo.x, v.x);
				lo.y = std::min(lo.y, v.y);
				lo.z = std::min(lo.z, v.z);
				hi.x = std::max(hi.x, v.x);
				hi.y = std::max(hi.y, v.y);
				hi.z = std::max(hi.z, v.z);
			}
			double ax = va.x - o.x, ay = va.y - o.y, az = va.z - o.z;
			double bx = vb.x - o.x, by = vb.y - o.y, bz = vb.z - o.z;
			double cx = vc.x - o.x, cy = vc.y - o.y, cz = vc.z - o.z;
			double cross_x = by * cz - bz * cy;
			double cross_y = bz * cx - bx * cz;
			double cross_z = bx * cy - by * cx;
			out.signed_vol += ax * cross_x + ay * cross_y + az * cross_z;
		}
	}
	double dx = hi.x - lo.x, dy = hi.y - lo.y, dz = hi.z - lo.z;
	double diag = std::sqrt(dx * dx + dy * dy + dz * dz);
	out.scale = diag * diag * diag;
	return out;
}

//! Union-find over a small index range, reused across shells.
struct DisjointSets {
	std::vector<uint32_t> parent;
	void Reset(uint32_t n) {
		parent.resize(n);
		for (uint32_t i = 0; i < n; i++) {
			parent[i] = i;
		}
	}
	uint32_t Find(uint32_t x) {
		while (parent[x] != x) {
			parent[x] = parent[parent[x]];
			x = parent[x];
		}
		return x;
	}
	//! Returns true when x and y were in different sets.
	bool Union(uint32_t x, uint32_t y) {
		x = Find(x);
		y = Find(y);
		if (x == y) {
			return false;
		}
		parent[std::max(x, y)] = std::min(x, y);
		return true;
	}
};

//! One ring corner: vertex `vertex` of face `face` (shell-local), with its
//! neighbours along the ring.
struct Corner {
	uint32_t vertex;
	uint32_t face;
	uint32_t prev;
	uint32_t next;
};

struct TopologyScratch {
	std::vector<std::pair<uint64_t, uint32_t>> edge_faces; //!< (undirected edge key, shell-local face)
	std::vector<Corner> corners;
	std::vector<std::pair<uint32_t, uint32_t>> spokes; //!< (other end of an edge at the vertex, face)
	std::vector<uint32_t> fan_faces;
	std::vector<uint32_t> roots;
	DisjointSets components;
	DisjointSets fans;
};

//! Connectivity and vertex manifoldness of one shell. Faces are joined when
//! they share an edge; a shell whose faces form more than one such part is
//! disconnected (val3dity 305). Around each vertex, faces sharing an edge at
//! that vertex form a fan; a vertex with more fans than the parts its faces
//! belong to is pinched (val3dity 303). Counting fans per part keeps a vertex
//! where two separate parts touch out of the count: that shell is already
//! disconnected.
void CheckShellConnectivity(const SolidModel &model, uint32_t shell_idx, TopologyScratch &t,
                            uint32_t &disconnected_shells, uint32_t &non_manifold_vertices) {
	uint32_t face_start = model.shell_face_offsets[shell_idx];
	uint32_t face_count = model.shell_face_offsets[shell_idx + 1] - face_start;
	if (face_count == 0) {
		return;
	}
	t.edge_faces.clear();
	t.corners.clear();
	for (uint32_t lf = 0; lf < face_count; lf++) {
		uint32_t f = face_start + lf;
		for (uint32_t ring_idx = model.face_ring_offsets[f]; ring_idx < model.face_ring_offsets[f + 1]; ring_idx++) {
			uint32_t vi_start = model.ring_vertex_offsets[ring_idx];
			uint32_t n = model.ring_vertex_offsets[ring_idx + 1] - vi_start;
			for (uint32_t i = 0; i < n; i++) {
				uint32_t v = model.ring_vertex_indices[vi_start + i];
				uint32_t next = model.ring_vertex_indices[vi_start + ((i + 1) % n)];
				uint32_t prev = model.ring_vertex_indices[vi_start + ((i + n - 1) % n)];
				uint64_t lo = std::min(v, next), hi = std::max(v, next);
				t.edge_faces.emplace_back((lo << 32) | hi, lf);
				t.corners.push_back({v, lf, prev, next});
			}
		}
	}

	t.components.Reset(face_count);
	std::sort(t.edge_faces.begin(), t.edge_faces.end());
	uint32_t parts = face_count;
	for (size_t i = 1; i < t.edge_faces.size(); i++) {
		if (t.edge_faces[i].first == t.edge_faces[i - 1].first &&
		    t.components.Union(t.edge_faces[i].second, t.edge_faces[i - 1].second)) {
			parts--;
		}
	}
	if (parts > 1) {
		disconnected_shells++;
	}

	std::sort(t.corners.begin(), t.corners.end(), [](const Corner &a, const Corner &b) { return a.vertex < b.vertex; });
	t.fans.Reset(face_count);
	for (size_t i = 0, n = t.corners.size(); i < n;) {
		size_t j = i;
		while (j < n && t.corners[j].vertex == t.corners[i].vertex) {
			j++;
		}
		if (j - i > 1) {
			t.spokes.clear();
			t.fan_faces.clear();
			for (size_t k = i; k < j; k++) {
				t.spokes.emplace_back(t.corners[k].prev, t.corners[k].face);
				t.spokes.emplace_back(t.corners[k].next, t.corners[k].face);
				t.fan_faces.push_back(t.corners[k].face);
			}
			std::sort(t.fan_faces.begin(), t.fan_faces.end());
			t.fan_faces.erase(std::unique(t.fan_faces.begin(), t.fan_faces.end()), t.fan_faces.end());
			std::sort(t.spokes.begin(), t.spokes.end());
			uint32_t fans = static_cast<uint32_t>(t.fan_faces.size());
			for (size_t k = 1; k < t.spokes.size(); k++) {
				if (t.spokes[k].first == t.spokes[k - 1].first &&
				    t.fans.Union(t.spokes[k].second, t.spokes[k - 1].second)) {
					fans--;
				}
			}
			// Parts the vertex's faces belong to.
			t.roots.clear();
			for (auto face : t.fan_faces) {
				t.roots.push_back(t.components.Find(face));
			}
			std::sort(t.roots.begin(), t.roots.end());
			auto vertex_parts = static_cast<uint32_t>(std::unique(t.roots.begin(), t.roots.end()) - t.roots.begin());
			if (fans > vertex_parts) {
				non_manifold_vertices++;
			}
			// Restore the fan sets touched for this vertex.
			for (auto face : t.fan_faces) {
				t.fans.parent[face] = face;
			}
		}
		i = j;
	}
}

//! Enforce CityGML §9.3's interior-opposite-exterior winding within each solid
//! (DESIGN_DOC §8.1 / §8.2). Shell 0 is the exterior (CityJSON writes the
//! outer shell first, §7.1); every interior shell MUST be wound opposite to it,
//! else its volume would silently add instead of subtract. Two error classes:
//!   * an interior shell wound the SAME way as the exterior;
//!   * an interior shell that is not a cavity: its |signed volume| is at least
//!     the exterior's, it lies outside the exterior, or it lies inside another
//!     interior shell (its volume would be subtracted twice).
//! The exterior's absolute orientation (outward vs inward) is out of scope —
//! this guards volume integrity, the property ComputeVolume depends on.
//! Containment is decided by winding number on the cached triangles; shells
//! that cross are left to the self-intersection check. The check assumes
//! closed, consistently-oriented shells (validated separately) and is a no-op
//! for single-shell solids. Returns the number of shells in error.
//! Generalised winding number of shell `shell` about point `p`: the solid angle
//! its triangles subtend at p over 4*pi (van Oosterom and Strackee). About +-1
//! inside a closed shell, 0 outside, and near +-0.5 on its surface. Taken
//! relative to p, so it does not depend on where the model sits.
double ShellWindingNumber(const SolidModel &model, uint32_t shell, const Vertex3D &p) {
	double total = 0.0;
	for (uint32_t f = model.shell_face_offsets[shell]; f < model.shell_face_offsets[shell + 1]; f++) {
		for (uint32_t t = model.face_triangle_offsets[f]; t < model.face_triangle_offsets[f + 1]; t++) {
			Vertex3D v[3];
			double len[3];
			for (int k = 0; k < 3; k++) {
				const auto &q = model.vertices[model.triangle_vertex_indices[t * 3 + k]];
				v[k] = {q.x - p.x, q.y - p.y, q.z - p.z};
				len[k] = std::sqrt(v[k].x * v[k].x + v[k].y * v[k].y + v[k].z * v[k].z);
			}
			auto dot = [](const Vertex3D &a, const Vertex3D &b) {
				return a.x * b.x + a.y * b.y + a.z * b.z;
			};
			double det = v[0].x * (v[1].y * v[2].z - v[1].z * v[2].y) - v[0].y * (v[1].x * v[2].z - v[1].z * v[2].x) +
			             v[0].z * (v[1].x * v[2].y - v[1].y * v[2].x);
			double denom = len[0] * len[1] * len[2] + dot(v[0], v[1]) * len[2] + dot(v[1], v[2]) * len[0] +
			               dot(v[2], v[0]) * len[1];
			total += 2.0 * std::atan2(det, denom);
		}
	}
	return total / (4.0 * 3.14159265358979323846);
}

//! Whether shell `inner` lies inside shell `outer`, judged by the winding number
//! of `outer` about `inner`'s vertices. Called on shells that do not cross, so
//! every vertex of `inner` off `outer`'s surface gives the same answer; vertices
//! on the surface (|w| near 0.5) are skipped. A shell with no clear vertex (all
//! of them on `outer`) counts as inside.
bool ShellInside(const SolidModel &model, uint32_t inner, uint32_t outer) {
	uint32_t f_begin = model.shell_face_offsets[inner];
	uint32_t f_end = model.shell_face_offsets[inner + 1];
	if (f_begin == f_end) {
		return true;
	}
	uint32_t r_begin = model.face_ring_offsets[f_begin];
	uint32_t r_end = model.face_ring_offsets[f_end];
	for (uint32_t i = model.ring_vertex_offsets[r_begin]; i < model.ring_vertex_offsets[r_end]; i++) {
		double w = std::abs(ShellWindingNumber(model, outer, model.vertices[model.ring_vertex_indices[i]]));
		if (w < 0.25) {
			return false;
		}
		if (w > 0.75) {
			return true;
		}
	}
	return true;
}

uint32_t CheckInteriorShellWinding(const SolidModel &model) {
	uint32_t errors = 0;
	uint32_t solid_count = model.SolidCount();

	for (uint32_t solid_idx = 0; solid_idx < solid_count; solid_idx++) {
		uint32_t shell_start = model.solid_shell_offsets[solid_idx];
		uint32_t shell_end = model.solid_shell_offsets[solid_idx + 1];
		if (shell_end - shell_start < 2) {
			continue; // no interior shells → nothing to compare
		}

		auto ext = ComputeShellSignedVolume(model, shell_start);
		// A degenerate/near-zero exterior gives no reliable sign; leave it to the
		// topology/degeneracy checks rather than guessing here. Degeneracy is
		// measured against the shell's own bbox scale, so a valid concave shell
		// (small volume, large surface) is not misjudged as degenerate.
		if (!ext.has_geometry || ext.scale == 0.0 || std::abs(ext.signed_vol) < kEpsRelative * ext.scale) {
			continue;
		}
		bool ext_positive = ext.signed_vol > 0.0;
		double ext_mag = std::abs(ext.signed_vol);

		for (uint32_t s = shell_start + 1; s < shell_end; s++) {
			auto in = ComputeShellSignedVolume(model, s);
			if (!in.has_geometry || in.scale == 0.0 || std::abs(in.signed_vol) < kEpsRelative * in.scale) {
				continue; // degenerate interior shell — sign is noise, skip
			}
			bool in_positive = in.signed_vol > 0.0;
			if (in_positive == ext_positive) {
				errors++; // same winding as exterior → cavity would add, not subtract
			} else if (std::abs(in.signed_vol) >= ext_mag) {
				errors++; // larger than the exterior → cannot be an interior cavity
			} else if (!ShellInside(model, s, shell_start)) {
				errors++; // outside the exterior → nothing to subtract from
			} else {
				for (uint32_t other = shell_start + 1; other < shell_end; other++) {
					if (other != s && ShellInside(model, s, other) && !ShellInside(model, other, s)) {
						errors++; // inside another cavity → subtracted twice
						break;
					}
				}
			}
		}
	}
	return errors;
}

} // anonymous namespace

void ValidateSolidModel(SolidModel &model) {
	ValidationCache &vc = model.validation;
	vc = {}; // reset

	uint32_t total_degenerate = 0;
	uint32_t total_open = 0;
	uint32_t total_non_manifold = 0;
	uint32_t total_orientation_errors = 0;
	bool all_closed = true;
	bool all_manifold = true;
	bool all_oriented = true;

	// Check for degenerate faces across all faces
	uint32_t face_count = model.FaceCount();
	for (uint32_t f = 0; f < face_count; f++) {
		if (IsFaceDegenerate(model, f)) {
			total_degenerate++;
		}
	}

	// Validate each shell
	uint32_t shell_count = model.ShellCount();
	EdgeScratch edges;
	TopologyScratch topology;
	uint32_t disconnected_shells = 0;
	uint32_t non_manifold_vertices = 0;
	for (uint32_t s = 0; s < shell_count; s++) {
		CheckShellConnectivity(model, s, topology, disconnected_shells, non_manifold_vertices);
		auto result = ValidateShellTopology(model, s, edges);
		total_open += result.open_edges;
		total_non_manifold += result.non_manifold_edges;
		total_orientation_errors += result.orientation_errors;
		if (!result.is_closed) {
			all_closed = false;
		}
		if (!result.is_manifold) {
			all_manifold = false;
		}
		if (!result.is_oriented) {
			all_oriented = false;
		}
	}

	// Cross-shell winding (CityGML §9.3): interior shells must be wound opposite
	// the exterior, else a mis-wound cavity's volume would silently add. Requires
	// the triangulation; when absent (never at build time) it is skipped.
	if (model.TriangleCount() > 0) {
		uint32_t winding_errors = CheckInteriorShellWinding(model);
		total_orientation_errors += winding_errors;
		if (winding_errors > 0) {
			all_oriented = false;
		}
	}

	// Faces crossing each other, within a shell or across the shells of a
	// solid. Like the winding check, it needs the triangulation.
	uint32_t self_intersections = model.TriangleCount() > 0 ? CountSelfIntersections(model) : 0;

	vc.open_edge_count = total_open;
	vc.non_manifold_edge_count = total_non_manifold;
	vc.degenerate_face_count = total_degenerate;
	vc.orientation_error_count = total_orientation_errors;
	vc.non_manifold_vertex_count = non_manifold_vertices;
	vc.disconnected_shell_count = disconnected_shells;
	vc.self_intersection_count = self_intersections;
	vc.is_closed = all_closed;
	vc.is_manifold = all_manifold && non_manifold_vertices == 0;
	vc.is_oriented = all_oriented;
	vc.is_valid = vc.is_closed && vc.is_manifold && all_oriented && total_degenerate == 0 && disconnected_shells == 0 &&
	              self_intersections == 0;
}

} // namespace duckdb_3d
