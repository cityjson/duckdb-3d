#include "kernel/model_builder.hpp"
#include <unordered_map>
#include <cmath>
#include <functional>

namespace duckdb_3d {

namespace {

//! Hash function for Vertex3D to support deduplication via unordered_map
struct Vertex3DHash {
	size_t operator()(const Vertex3D &v) const {
		// Combine hashes of x, y, z using a standard approach
		size_t h1 = std::hash<double>{}(v.x);
		size_t h2 = std::hash<double>{}(v.y);
		size_t h3 = std::hash<double>{}(v.z);
		h1 ^= h2 + 0x9e3779b9 + (h1 << 6) + (h1 >> 2);
		h1 ^= h3 + 0x9e3779b9 + (h1 << 6) + (h1 >> 2);
		return h1;
	}
};

//! Returns true if two consecutive vertices are duplicates (exact match)
bool IsConsecutiveDuplicate(const Vertex3D &a, const Vertex3D &b) {
	return a.x == b.x && a.y == b.y && a.z == b.z;
}

} // anonymous namespace

SolidModel BuildSolidModel(const std::vector<ParsedPolyhedralSurface> &surfaces) {
	SolidModel model;

	// Global vertex deduplication map
	std::unordered_map<Vertex3D, uint32_t, Vertex3DHash> vertex_map;

	auto GetOrAddVertex = [&](const Vertex3D &v) -> uint32_t {
		auto it = vertex_map.find(v);
		if (it != vertex_map.end()) {
			return it->second;
		}
		uint32_t idx = static_cast<uint32_t>(model.vertices.size());
		model.vertices.push_back(v);
		vertex_map[v] = idx;
		return idx;
	};

	// Each ParsedPolyhedralSurface becomes one solid with one shell (plain WKB)
	uint32_t total_shells = 0;
	uint32_t total_faces = 0;
	uint32_t total_rings = 0;

	model.solid_shell_offsets.push_back(0);

	for (const auto &surface : surfaces) {
		// One shell per solid
		uint32_t shell_start_face = total_faces;
		model.shell_face_offsets.push_back(total_faces);

		// Walk through the surface's vertices using ring_vertex_counts
		size_t vertex_cursor = 0;
		size_t ring_idx = 0;

		for (uint32_t p = 0; p < surface.polygon_count; p++) {
			uint32_t num_rings = surface.polygon_ring_counts[p];
			model.face_ring_offsets.push_back(total_rings);

			for (uint32_t r = 0; r < num_rings; r++) {
				uint32_t ring_vcount = surface.ring_vertex_counts[ring_idx];
				model.ring_vertex_offsets.push_back(static_cast<uint32_t>(model.ring_vertex_indices.size()));

				// Collect ring vertices, removing consecutive duplicates
				Vertex3D prev = {};
				bool has_prev = false;

				for (uint32_t vi = 0; vi < ring_vcount; vi++) {
					const Vertex3D &v = surface.vertices[vertex_cursor + vi];
					if (has_prev && IsConsecutiveDuplicate(prev, v)) {
						continue; // skip consecutive duplicate
					}
					uint32_t idx = GetOrAddVertex(v);
					model.ring_vertex_indices.push_back(idx);
					prev = v;
					has_prev = true;
				}

				vertex_cursor += ring_vcount;
				ring_idx++;
				total_rings++;
			}

			total_faces++;
		}

		total_shells++;
		model.solid_shell_offsets.push_back(total_shells);
	}

	// Close remaining offset arrays
	model.shell_face_offsets.push_back(total_faces);
	model.face_ring_offsets.push_back(total_rings);
	model.ring_vertex_offsets.push_back(static_cast<uint32_t>(model.ring_vertex_indices.size()));

	// Triangulation cache: placeholder (empty for now, filled in Phase 3)
	model.face_triangle_offsets.resize(total_faces + 1, 0);

	model.ComputeBBox();

	return model;
}

} // namespace duckdb_3d
