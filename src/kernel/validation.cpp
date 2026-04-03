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
		size_t h = std::hash<uint32_t>{}(e.a);
		h ^= std::hash<uint32_t>{}(e.b) + 0x9e3779b9 + (h << 6) + (h >> 2);
		return h;
	}
};

//! Cross product of (b-a) x (c-a)
Vertex3D Cross(const Vertex3D &a, const Vertex3D &b, const Vertex3D &c) {
	double ux = b.x - a.x, uy = b.y - a.y, uz = b.z - a.z;
	double vx = c.x - a.x, vy = c.y - a.y, vz = c.z - a.z;
	return {uy * vz - uz * vy, uz * vx - ux * vz, ux * vy - uy * vx};
}

double Magnitude(const Vertex3D &v) {
	return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
}

//! Check if a face is degenerate (area near zero).
//! Uses the sum of cross products over the ring to compute face normal area.
bool IsFaceDegenerate(const SolidModel &model, uint32_t face_idx) {
	uint32_t ring_start = model.face_ring_offsets[face_idx];
	// Only check exterior ring for degeneracy
	uint32_t vi_start = model.ring_vertex_offsets[ring_start];
	uint32_t vi_end = model.ring_vertex_offsets[ring_start + 1];
	uint32_t n = vi_end - vi_start;

	if (n < 3) {
		return true;
	}

	// Newell's method for polygon normal/area
	double nx = 0, ny = 0, nz = 0;
	for (uint32_t i = 0; i < n; i++) {
		uint32_t idx_cur = model.ring_vertex_indices[vi_start + i];
		uint32_t idx_next = model.ring_vertex_indices[vi_start + ((i + 1) % n)];
		const auto &cur = model.vertices[idx_cur];
		const auto &next = model.vertices[idx_next];
		nx += (cur.y - next.y) * (cur.z + next.z);
		ny += (cur.z - next.z) * (cur.x + next.x);
		nz += (cur.x - next.x) * (cur.y + next.y);
	}

	double area = 0.5 * std::sqrt(nx * nx + ny * ny + nz * nz);
	return area < EPSILON;
}

//! Collect directed edges from a shell's faces
void CollectShellEdges(const SolidModel &model, uint32_t shell_idx,
                       std::vector<DirectedEdge> &directed_edges) {
	uint32_t face_start = model.shell_face_offsets[shell_idx];
	uint32_t face_end = model.shell_face_offsets[shell_idx + 1];

	for (uint32_t f = face_start; f < face_end; f++) {
		uint32_t ring_start = model.face_ring_offsets[f];
		// Only analyze the exterior ring for edge topology
		uint32_t vi_start = model.ring_vertex_offsets[ring_start];
		uint32_t vi_end = model.ring_vertex_offsets[ring_start + 1];
		uint32_t n = vi_end - vi_start;

		for (uint32_t i = 0; i < n; i++) {
			uint32_t from = model.ring_vertex_indices[vi_start + i];
			uint32_t to = model.ring_vertex_indices[vi_start + ((i + 1) % n)];
			directed_edges.push_back({from, to});
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
			size_t h = std::hash<uint32_t>{}(p.first);
			h ^= std::hash<uint32_t>{}(p.second) + 0x9e3779b9 + (h << 6) + (h >> 2);
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
