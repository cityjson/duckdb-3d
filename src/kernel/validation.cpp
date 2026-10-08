#include "kernel/validation.hpp"
#include "kernel/geometry_math.hpp"
#include <algorithm>
#include <cmath>
#include <utility>

namespace duckdb_3d {

namespace {

double Magnitude(const Vertex3D &v) {
	return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
}

//! Check if a face is degenerate: fewer than 3 distinct vertices in a ring, an
//! area near zero (the sum of all ring area vectors, to account for holes), or
//! a triangulation that failed.
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
		if (Magnitude(ring_area) < kEpsAbsolute) {
			return true;
		}

		face_area.x += ring_area.x;
		face_area.y += ring_area.y;
		face_area.z += ring_area.z;
	}

	if (Magnitude(face_area) < kEpsAbsolute) {
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

//! Enforce CityGML §9.3's interior-opposite-exterior winding within each solid
//! (DESIGN_DOC §8.1 / §8.2). Shell 0 is the exterior (CityJSON writes the
//! outer shell first, §7.1); every interior shell MUST be wound opposite to it,
//! else its volume would silently add instead of subtract. Two error classes:
//!   * an interior shell wound the SAME way as the exterior;
//!   * an interior shell whose |signed volume| >= the exterior's (it cannot be
//!     contained, so it is not a real cavity).
//! Scope is deliberately RELATIVE: the exterior's absolute orientation (outward
//! vs inward) and true point-in-polyhedron containment are out of scope — this
//! guards volume integrity, the property ComputeVolume depends on. The check
//! assumes closed, consistently-oriented shells (validated separately) and is a
//! no-op for single-shell solids. Returns the number of orientation errors found.
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
	for (uint32_t s = 0; s < shell_count; s++) {
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

	vc.open_edge_count = total_open;
	vc.non_manifold_edge_count = total_non_manifold;
	vc.degenerate_face_count = total_degenerate;
	vc.orientation_error_count = total_orientation_errors;
	vc.is_closed = all_closed;
	vc.is_manifold = all_manifold;
	vc.is_oriented = all_oriented;
	vc.is_valid = all_closed && all_manifold && all_oriented && (total_degenerate == 0);
}

} // namespace duckdb_3d
