#include "kernel/validation.hpp"
#include <unordered_map>
#include <algorithm>
#include <cmath>
#include <utility>

namespace duckdb_3d {

namespace {

//! An ordered edge (v_from, v_to)
struct DirectedEdge {
	uint32_t from;
	uint32_t to;
};

//! An unordered edge for counting (min, max)
struct UndirectedEdge {
	uint32_t a;
	uint32_t b;

	UndirectedEdge(uint32_t from, uint32_t to) : a(std::min(from, to)), b(std::max(from, to)) {
	}

	bool operator==(const UndirectedEdge &other) const {
		return a == other.a && b == other.b;
	}
};

struct UndirectedEdgeHash {
	size_t operator()(const UndirectedEdge &e) const {
		size_t h = std::hash<uint32_t> {}(e.a);
		h ^= std::hash<uint32_t> {}(e.b) + 0x9e3779b9 + (h << 6) + (h >> 2);
		return h;
	}
};

double Magnitude(const Vertex3D &v) {
	return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
}

Vertex3D ComputeRingAreaVector(const SolidModel &model, uint32_t ring_idx) {
	uint32_t vi_start = model.ring_vertex_offsets[ring_idx];
	uint32_t vi_end = model.ring_vertex_offsets[ring_idx + 1];
	uint32_t n = vi_end - vi_start;

	Vertex3D area = {0, 0, 0};
	if (n < 3) {
		return area;
	}

	for (uint32_t i = 0; i < n; i++) {
		uint32_t idx_cur = model.ring_vertex_indices[vi_start + i];
		uint32_t idx_next = model.ring_vertex_indices[vi_start + ((i + 1) % n)];
		const auto &cur = model.vertices[idx_cur];
		const auto &next = model.vertices[idx_next];
		area.x += (cur.y - next.y) * (cur.z + next.z);
		area.y += (cur.z - next.z) * (cur.x + next.x);
		area.z += (cur.x - next.x) * (cur.y + next.y);
	}

	return area;
}

//! Check if a face is degenerate (area near zero).
//! Uses the sum of all ring area vectors to account for faces with holes.
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

		auto ring_area = ComputeRingAreaVector(model, ring_idx);
		if (Magnitude(ring_area) < EPSILON) {
			return true;
		}

		face_area.x += ring_area.x;
		face_area.y += ring_area.y;
		face_area.z += ring_area.z;
	}

	return Magnitude(face_area) < EPSILON;
}

//! Collect directed edges from a shell's faces
void CollectShellEdges(const SolidModel &model, uint32_t shell_idx, std::vector<DirectedEdge> &directed_edges) {
	uint32_t face_start = model.shell_face_offsets[shell_idx];
	uint32_t face_end = model.shell_face_offsets[shell_idx + 1];

	for (uint32_t f = face_start; f < face_end; f++) {
		uint32_t ring_start = model.face_ring_offsets[f];
		uint32_t ring_end = model.face_ring_offsets[f + 1];

		for (uint32_t ring_idx = ring_start; ring_idx < ring_end; ring_idx++) {
			uint32_t vi_start = model.ring_vertex_offsets[ring_idx];
			uint32_t vi_end = model.ring_vertex_offsets[ring_idx + 1];
			uint32_t n = vi_end - vi_start;

			for (uint32_t i = 0; i < n; i++) {
				uint32_t from = model.ring_vertex_indices[vi_start + i];
				uint32_t to = model.ring_vertex_indices[vi_start + ((i + 1) % n)];
				directed_edges.push_back({from, to});
			}
		}
	}
}

struct ShellValidationResult {
	uint32_t open_edges = 0;
	uint32_t non_manifold_edges = 0;
	uint32_t orientation_errors = 0;
	bool is_closed = true;
	bool is_manifold = true;
	bool is_oriented = true;
};

ShellValidationResult ValidateShellTopology(const SolidModel &model, uint32_t shell_idx) {
	ShellValidationResult result;

	std::vector<DirectedEdge> directed_edges;
	CollectShellEdges(model, shell_idx, directed_edges);

	// Count directed edges per undirected edge
	// For closedness: each undirected edge should appear exactly 2 times total
	// For orientation: each directed edge (a,b) should have a matching (b,a)
	std::unordered_map<UndirectedEdge, uint32_t, UndirectedEdgeHash> undirected_count;
	// Count directed edge occurrences using a pair hash
	struct PairHash {
		size_t operator()(const std::pair<uint32_t, uint32_t> &p) const {
			size_t h = std::hash<uint32_t> {}(p.first);
			h ^= std::hash<uint32_t> {}(p.second) + 0x9e3779b9 + (h << 6) + (h >> 2);
			return h;
		}
	};
	std::unordered_map<std::pair<uint32_t, uint32_t>, uint32_t, PairHash> directed_count;

	for (auto &e : directed_edges) {
		undirected_count[UndirectedEdge(e.from, e.to)]++;
		directed_count[{e.from, e.to}]++;
	}

	for (auto it = undirected_count.begin(); it != undirected_count.end(); ++it) {
		if (it->second < 2) {
			result.open_edges++;
			result.is_closed = false;
		} else if (it->second > 2) {
			result.non_manifold_edges++;
			result.is_manifold = false;
		}
	}

	// Orientation check: for each directed edge (a,b), the reverse (b,a) should exist exactly once
	for (auto dit = directed_count.begin(); dit != directed_count.end(); ++dit) {
		auto reverse = std::make_pair(dit->first.second, dit->first.first);
		auto rit = directed_count.find(reverse);
		if (dit->second > 1) {
			// Same directed edge appears more than once — orientation error or non-manifold
			result.orientation_errors++;
			result.is_oriented = false;
		} else if (rit == directed_count.end() || rit->second == 0) {
			// No reverse edge — this is an orientation issue (if closed, edges should cancel)
			// But only flag as orientation error if the edge exists twice undirected
			UndirectedEdge ue(dit->first.first, dit->first.second);
			auto uit = undirected_count.find(ue);
			if (uit != undirected_count.end() && uit->second == 2) {
				// Edge exists twice but both in same direction — orientation error
				result.orientation_errors++;
				result.is_oriented = false;
			}
		}
	}

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
	double abs_sum = 0.0;
};

ShellSignedVolume ComputeShellSignedVolume(const SolidModel &model, uint32_t shell_idx) {
	ShellSignedVolume out;
	uint32_t face_start = model.shell_face_offsets[shell_idx];
	uint32_t face_end = model.shell_face_offsets[shell_idx + 1];

	// Local origin: the shell's first triangulated vertex.
	bool have_origin = false;
	Vertex3D o = {0, 0, 0};
	for (uint32_t f = face_start; f < face_end && !have_origin; f++) {
		uint32_t tri_start = model.face_triangle_offsets[f];
		uint32_t tri_end = model.face_triangle_offsets[f + 1];
		if (tri_start < tri_end) {
			o = model.vertices[model.triangle_vertex_indices[tri_start * 3 + 0]];
			have_origin = true;
		}
	}
	if (!have_origin) {
		return out; // no triangles → nothing to integrate
	}

	for (uint32_t f = face_start; f < face_end; f++) {
		uint32_t tri_start = model.face_triangle_offsets[f];
		uint32_t tri_end = model.face_triangle_offsets[f + 1];
		for (uint32_t t = tri_start; t < tri_end; t++) {
			const auto &va = model.vertices[model.triangle_vertex_indices[t * 3 + 0]];
			const auto &vb = model.vertices[model.triangle_vertex_indices[t * 3 + 1]];
			const auto &vc = model.vertices[model.triangle_vertex_indices[t * 3 + 2]];
			double ax = va.x - o.x, ay = va.y - o.y, az = va.z - o.z;
			double bx = vb.x - o.x, by = vb.y - o.y, bz = vb.z - o.z;
			double cx = vc.x - o.x, cy = vc.y - o.y, cz = vc.z - o.z;
			double cross_x = by * cz - bz * cy;
			double cross_y = bz * cx - bx * cz;
			double cross_z = bx * cy - by * cx;
			double tv = ax * cross_x + ay * cross_y + az * cross_z;
			out.signed_vol += tv;
			out.abs_sum += std::abs(tv);
		}
	}
	return out;
}

//! Enforce CityGML §9.3's interior-opposite-exterior winding within each solid
//! (DESIGN_DOC §9.3 / §10.2.1). Shell 0 is the exterior (CityJSON writes the
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
	constexpr double kRelEps = 1e-9; // relative degeneracy tolerance
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
		// topology/degeneracy checks rather than guessing here.
		if (ext.abs_sum == 0.0 || std::abs(ext.signed_vol) < kRelEps * ext.abs_sum) {
			continue;
		}
		bool ext_positive = ext.signed_vol > 0.0;
		double ext_mag = std::abs(ext.signed_vol);

		for (uint32_t s = shell_start + 1; s < shell_end; s++) {
			auto in = ComputeShellSignedVolume(model, s);
			if (in.abs_sum == 0.0 || std::abs(in.signed_vol) < kRelEps * in.abs_sum) {
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
	for (uint32_t s = 0; s < shell_count; s++) {
		auto result = ValidateShellTopology(model, s);
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
