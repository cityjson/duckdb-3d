#include "kernel/model_builder.hpp"
#include "kernel/validation.hpp"
#include "kernel/triangulation.hpp"
#include <cstring>
#include <stdexcept>

namespace duckdb_3d {

namespace {

//! Maps each distinct vertex to its index in the model's vertex array, in order
//! of first appearance. An open-addressing table sized once for the surface's
//! vertex count (an upper bound on the distinct ones), so it never rehashes and
//! allocates once per model. Vertices are equal when operator== says so, as for
//! an unordered_map: +0 and -0 match, and a NaN coordinate matches nothing, so
//! each such vertex gets its own index.
class VertexDedup {
public:
	explicit VertexDedup(size_t max_vertices) {
		size_t capacity = 16;
		while (capacity < 2 * max_vertices) {
			capacity <<= 1;
		}
		slots.assign(capacity, kEmpty);
		mask = capacity - 1;
	}

	uint32_t GetOrAdd(const Vertex3D &v, std::vector<Vertex3D> &vertices) {
		for (size_t h = Hash(v) & mask;; h = (h + 1) & mask) {
			uint32_t slot = slots[h];
			if (slot == kEmpty) {
				auto idx = static_cast<uint32_t>(vertices.size());
				vertices.push_back(v);
				slots[h] = idx;
				return idx;
			}
			if (vertices[slot] == v) {
				return slot;
			}
		}
	}

private:
	static constexpr uint32_t kEmpty = UINT32_MAX;
	std::vector<uint32_t> slots;
	size_t mask;

	static uint64_t Bits(double d) {
		if (d == 0.0) {
			d = 0.0; // -0 == +0, so both must hash alike
		}
		uint64_t b;
		std::memcpy(&b, &d, sizeof(b));
		return b;
	}
	static size_t Hash(const Vertex3D &v) {
		uint64_t h = Bits(v.x) * 0x9e3779b97f4a7c15ULL;
		h = (h ^ (h >> 29) ^ Bits(v.y)) * 0xbf58476d1ce4e5b9ULL;
		h = (h ^ (h >> 31) ^ Bits(v.z)) * 0x94d049bb133111ebULL;
		return static_cast<size_t>(h ^ (h >> 32));
	}
};

//! Upper bound on the distinct vertices of `surfaces`: all their ring points.
size_t TotalVertices(const std::vector<ParsedPolyhedralSurface> &surfaces) {
	size_t n = 0;
	for (const auto &surface : surfaces) {
		n += surface.vertices.size();
	}
	return n;
}

//! Returns true if two consecutive vertices are duplicates (exact match)
bool IsConsecutiveDuplicate(const Vertex3D &a, const Vertex3D &b) {
	return a.x == b.x && a.y == b.y && a.z == b.z;
}

} // anonymous namespace

SolidModel BuildSolidModel(const std::vector<ParsedPolyhedralSurface> &surfaces) {
	SolidModel model;

	// Global vertex deduplication
	size_t total_vertices = TotalVertices(surfaces);
	VertexDedup dedup(total_vertices);
	model.ring_vertex_indices.reserve(total_vertices);
	auto GetOrAddVertex = [&](const Vertex3D &v) -> uint32_t {
		return dedup.GetOrAdd(v, model.vertices);
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

	// Triangulation cache: sized here, filled by TriangulateSolidModel below.
	model.face_triangle_offsets.resize(total_faces + 1, 0);

	model.ComputeBBox();
	TriangulateSolidModel(model);
	ValidateSolidModel(model);

	return model;
}

SolidModel BuildSolidModel(const std::vector<ParsedPolyhedralSurface> &surfaces, const GeometryMetadata &metadata) {
	// No `shells` metadata: one solid / one shell per WKB member (a plain Solid,
	// or a MultiSolid/CompositeSolid whose solids have no inner shells).
	if (!metadata.shells.has_value()) {
		return BuildSolidModel(surfaces);
	}

	// With `shells`, one per-shell-count array must map to one WKB member (solid).
	// A Solid gives a single member; a MultiSolid/CompositeSolid one per solid.
	const auto &all_shells = *metadata.shells;
	if (all_shells.size() != surfaces.size()) {
		throw std::runtime_error("geometry_properties: shells solid count (" + std::to_string(all_shells.size()) +
		                         ") does not match WKB member count (" + std::to_string(surfaces.size()) + ")");
	}

	SolidModel model;

	// Global vertex deduplication
	size_t total_vertices = TotalVertices(surfaces);
	VertexDedup dedup(total_vertices);
	model.ring_vertex_indices.reserve(total_vertices);
	auto GetOrAddVertex = [&](const Vertex3D &v) -> uint32_t {
		return dedup.GetOrAdd(v, model.vertices);
	};

	uint32_t total_faces = 0;
	uint32_t total_rings = 0;
	uint32_t total_shells = 0;

	model.solid_shell_offsets.push_back(0);

	// Each surface is one solid; its faces are partitioned into shells by the
	// matching per-shell face-count array (spec §8 `shells`).
	for (size_t solid_idx = 0; solid_idx < surfaces.size(); solid_idx++) {
		const auto &surface = surfaces[solid_idx];
		const auto &shell_counts = all_shells[solid_idx];

		// Accumulate in 64-bit so a crafted count near UINT32_MAX cannot wrap the
		// sum back to a value that spuriously matches the WKB face count.
		uint64_t face_sum = 0;
		uint32_t non_empty_shells = 0;
		for (auto fc : shell_counts) {
			face_sum += fc;
			if (fc > 0) {
				non_empty_shells++;
			}
		}
		if (face_sum != surface.polygon_count) {
			throw std::runtime_error("geometry_properties: shell face count mismatch for solid " +
			                         std::to_string(solid_idx) + ": shells sum (" + std::to_string(face_sum) +
			                         ") != WKB face count (" + std::to_string(surface.polygon_count) + ")");
		}
		// A `0` in `shells` is a fully-dropped shell (spec §8) and creates no shell;
		// a solid must still have at least one real shell.
		if (non_empty_shells == 0) {
			throw std::runtime_error("geometry_properties: solid " + std::to_string(solid_idx) +
			                         " has no non-empty shell");
		}

		// Walk this surface's faces in order, marking a shell boundary at each
		// per-shell count. vertex_cursor / ring_idx walk the surface's own arrays.
		size_t vertex_cursor = 0;
		size_t ring_idx = 0;
		uint32_t face_in_surface = 0;

		for (uint32_t shell_faces : shell_counts) {
			if (shell_faces == 0) {
				continue; // dropped shell — contributes no faces and no shell entry
			}
			model.shell_face_offsets.push_back(total_faces);

			for (uint32_t f = 0; f < shell_faces; f++) {
				uint32_t p = face_in_surface;
				uint32_t num_rings = surface.polygon_ring_counts[p];
				model.face_ring_offsets.push_back(total_rings);

				for (uint32_t r = 0; r < num_rings; r++) {
					uint32_t ring_vcount = surface.ring_vertex_counts[ring_idx];
					model.ring_vertex_offsets.push_back(static_cast<uint32_t>(model.ring_vertex_indices.size()));

					Vertex3D prev = {};
					bool has_prev = false;

					for (uint32_t vi = 0; vi < ring_vcount; vi++) {
						const Vertex3D &v = surface.vertices[vertex_cursor + vi];
						if (has_prev && IsConsecutiveDuplicate(prev, v)) {
							continue;
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

				face_in_surface++;
				total_faces++;
			}

			total_shells++;
		}

		model.solid_shell_offsets.push_back(total_shells);
	}

	// Close remaining offset arrays
	model.shell_face_offsets.push_back(total_faces);
	model.face_ring_offsets.push_back(total_rings);
	model.ring_vertex_offsets.push_back(static_cast<uint32_t>(model.ring_vertex_indices.size()));

	model.face_triangle_offsets.resize(total_faces + 1, 0);

	model.ComputeBBox();
	TriangulateSolidModel(model);
	ValidateSolidModel(model);

	return model;
}

} // namespace duckdb_3d
