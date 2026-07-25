#include "kernel/arrow_native_import.hpp"
#include "kernel/triangulation.hpp"
#include "kernel/validation.hpp"
#include <functional>
#include <stdexcept>
#include <unordered_map>

namespace duckdb_3d {

namespace {

//! Hash function for Vertex3D — mirrors model_builder.cpp's own combine
//! formula (kept local rather than shared: a private detail of vertex
//! deduplication, not part of either builder's public surface).
struct Vertex3DHash {
	size_t operator()(const Vertex3D &v) const {
		size_t h1 = std::hash<double> {}(v.x);
		size_t h2 = std::hash<double> {}(v.y);
		size_t h3 = std::hash<double> {}(v.z);
		h1 ^= h2 + 0x9e3779b9 + (h1 << 6) + (h1 >> 2);
		h1 ^= h3 + 0x9e3779b9 + (h1 << 6) + (h1 >> 2);
		return h1;
	}
};

} // namespace

SolidModel BuildSolidModelFromArrowNative(const ArrowNativeBoundaries &boundaries,
                                          const std::vector<Vertex3D> &vertices) {
	SolidModel model;

	// Defensive coordinate-equality dedup, mirroring model_builder.cpp's
	// GetOrAddVertex. This is a public SQL-callable ingestion boundary (unlike
	// an internal-only path), so a buggy writer that emits geometrically
	// duplicate vertices at distinct pool indices must not silently produce
	// mismatched edges downstream — ValidateSolidModel matches edges by vertex
	// INDEX, so two indices for "the same" point would wrongly read as an
	// open/non-manifold edge rather than a shared one.
	std::unordered_map<Vertex3D, uint32_t, Vertex3DHash> vertex_map;
	std::vector<uint32_t> remap(vertices.size());
	for (size_t i = 0; i < vertices.size(); i++) {
		auto it = vertex_map.find(vertices[i]);
		if (it != vertex_map.end()) {
			remap[i] = it->second;
		} else {
			uint32_t idx = static_cast<uint32_t>(model.vertices.size());
			model.vertices.push_back(vertices[i]);
			vertex_map[vertices[i]] = idx;
			remap[i] = idx;
		}
	}

	model.solid_shell_offsets = boundaries.solid_shell_offsets;
	model.shell_face_offsets = boundaries.shell_face_offsets;
	model.face_ring_offsets = boundaries.face_ring_offsets;
	model.ring_vertex_offsets = boundaries.ring_vertex_offsets;

	model.ring_vertex_indices.reserve(boundaries.ring_vertex_indices.size());
	for (uint32_t raw : boundaries.ring_vertex_indices) {
		if (raw >= vertices.size()) {
			throw std::runtime_error(
			    "arrow-native geometry: vertex-pool index out of range (design doc validity invariant)");
		}
		model.ring_vertex_indices.push_back(remap[raw]);
	}

	uint32_t face_count = model.FaceCount();
	model.face_triangle_offsets.resize(face_count + 1, 0);

	model.ComputeBBox();
	TriangulateSolidModel(model);
	ValidateSolidModel(model);

	return model;
}

} // namespace duckdb_3d
