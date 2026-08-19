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

	model.solid_shell_offsets = boundaries.solid_shell_offsets;
	model.shell_face_offsets = boundaries.shell_face_offsets;
	model.face_ring_offsets = boundaries.face_ring_offsets;

	// Defensive coordinate-equality dedup, mirroring model_builder.cpp's
	// GetOrAddVertex, but resolved lazily as each raw pool index is actually
	// referenced by a ring — not precomputed over the whole input pool. This
	// is a public SQL-callable ingestion boundary (unlike an internal-only
	// path), so two things must hold: (1) a buggy writer that emits
	// geometrically duplicate vertices at distinct pool indices must not
	// silently produce mismatched edges downstream (ValidateSolidModel
	// matches edges by vertex INDEX, so two indices for "the same" point
	// would wrongly read as an open/non-manifold edge rather than a shared
	// one); (2) a pool entry no ring ever references must never appear in
	// model.vertices — it would otherwise silently pollute ComputeBBox (and
	// anything else that iterates all model vertices) with a point outside
	// the actual geometry.
	std::unordered_map<Vertex3D, uint32_t, Vertex3DHash> vertex_map;
	std::unordered_map<uint32_t, uint32_t> remap; // raw pool index -> compacted model index

	auto ResolveCompactIndex = [&](uint32_t raw) -> uint32_t {
		if (raw >= vertices.size()) {
			throw std::runtime_error(
			    "arrow-native geometry: vertex-pool index out of range (design doc validity invariant)");
		}
		auto remap_it = remap.find(raw);
		if (remap_it != remap.end()) {
			return remap_it->second;
		}
		const Vertex3D &v = vertices[raw];
		auto vm_it = vertex_map.find(v);
		uint32_t compact_idx;
		if (vm_it != vertex_map.end()) {
			compact_idx = vm_it->second;
		} else {
			compact_idx = static_cast<uint32_t>(model.vertices.size());
			model.vertices.push_back(v);
			vertex_map[v] = compact_idx;
		}
		remap[raw] = compact_idx;
		return compact_idx;
	};

	// Walked ring-by-ring (not one flat pass over ring_vertex_indices) so a
	// consecutive-duplicate compact index — from a literal repeated raw
	// index, or from two distinct raw indices that dedup to the same
	// compact vertex — can be skipped per ring, mirroring
	// model_builder.cpp's IsConsecutiveDuplicate for the WKB path. Left
	// uncollapsed, a same-vertex "edge" is a zero-length self-loop with no
	// twin anywhere else in the model, which ValidateSolidModel would
	// misreport as an open edge. This can shrink a ring's index count, so
	// ring_vertex_offsets is rebuilt here rather than copied from boundaries.
	model.ring_vertex_indices.reserve(boundaries.ring_vertex_indices.size());
	model.ring_vertex_offsets.reserve(boundaries.ring_vertex_offsets.size());
	model.ring_vertex_offsets.push_back(0);
	if (!boundaries.ring_vertex_offsets.empty()) {
		for (size_t r = 0; r + 1 < boundaries.ring_vertex_offsets.size(); r++) {
			uint32_t raw_start = boundaries.ring_vertex_offsets[r];
			uint32_t raw_end = boundaries.ring_vertex_offsets[r + 1];
			bool has_prev = false;
			uint32_t prev_compact = 0;
			for (uint32_t k = raw_start; k < raw_end; k++) {
				uint32_t compact_idx = ResolveCompactIndex(boundaries.ring_vertex_indices[k]);
				if (has_prev && compact_idx == prev_compact) {
					continue; // skip consecutive duplicate
				}
				model.ring_vertex_indices.push_back(compact_idx);
				prev_compact = compact_idx;
				has_prev = true;
			}
			model.ring_vertex_offsets.push_back(static_cast<uint32_t>(model.ring_vertex_indices.size()));
		}
	}

	uint32_t face_count = model.FaceCount();
	model.face_triangle_offsets.resize(face_count + 1, 0);

	model.ComputeBBox();
	TriangulateSolidModel(model);
	ValidateSolidModel(model);

	return model;
}

SolidModel BuildSolidModelFromArrowNative(const ArrowNativeBoundaries &boundaries,
                                          const std::vector<Vertex3D> &vertices, const GeometryMetadata &metadata) {
	// No shells metadata: use the boundaries' own physical shell grouping
	// as-is (matches model_builder.cpp's BuildSolidModel(surfaces) with no
	// metadata).
	if (metadata.shells.empty()) {
		return BuildSolidModelFromArrowNative(boundaries, vertices);
	}

	uint32_t solid_count =
	    boundaries.solid_shell_offsets.empty() ? 0 : static_cast<uint32_t>(boundaries.solid_shell_offsets.size() - 1);
	if (metadata.shells.size() != solid_count) {
		throw std::runtime_error("arrow-native geometry_properties: shells solid count (" +
		                         std::to_string(metadata.shells.size()) + ") does not match boundaries solid count (" +
		                         std::to_string(solid_count) + ")");
	}

	// Regroup: a real producer (confirmed: cityparquet-rs's arrow_geom_write.rs)
	// always pads each solid to exactly one physical shell, flattening real
	// interior shells into a single face list exactly like the WKB path
	// flattens them into one PolyhedralSurface — the real per-solid shell
	// partition lives only in geometry_properties.shells. face_ring_offsets/
	// ring_vertex_offsets/ring_vertex_indices are untouched: regrouping only
	// reinterprets which shell boundary markers apply to the SAME faces,
	// never reorders them. Mirrors model_builder.cpp's BuildSolidModel(surfaces,
	// metadata) exactly: 64-bit sum to avoid overflow tricks, a 0 entry is a
	// fully-dropped shell creating no shell, at least one non-empty shell
	// required per solid.
	ArrowNativeBoundaries regrouped;
	regrouped.face_ring_offsets = boundaries.face_ring_offsets;
	regrouped.ring_vertex_offsets = boundaries.ring_vertex_offsets;
	regrouped.ring_vertex_indices = boundaries.ring_vertex_indices;
	regrouped.solid_shell_offsets.push_back(0);
	regrouped.shell_face_offsets.push_back(0);

	uint32_t total_shells = 0;
	for (uint32_t solid_idx = 0; solid_idx < solid_count; solid_idx++) {
		uint32_t padded_start = boundaries.solid_shell_offsets[solid_idx];
		uint32_t padded_end = boundaries.solid_shell_offsets[solid_idx + 1];
		if (padded_end - padded_start != 1) {
			throw std::runtime_error(
			    "arrow-native geometry: expected exactly one padded shell per solid when "
			    "geometry_properties.shells is present (a real producer flattens interior shells into it)");
		}
		uint32_t face_start = boundaries.shell_face_offsets[padded_start];
		uint32_t face_end = boundaries.shell_face_offsets[padded_start + 1];

		const auto &shell_counts = metadata.shells[solid_idx];
		uint64_t face_sum = 0;
		uint32_t non_empty_shells = 0;
		for (auto fc : shell_counts) {
			face_sum += fc;
			if (fc > 0) {
				non_empty_shells++;
			}
		}
		if (face_sum != face_end - face_start) {
			throw std::runtime_error("arrow-native geometry_properties: shell face count mismatch for solid " +
			                         std::to_string(solid_idx) + ": shells sum (" + std::to_string(face_sum) +
			                         ") != boundaries face count (" + std::to_string(face_end - face_start) + ")");
		}
		if (non_empty_shells == 0) {
			throw std::runtime_error("arrow-native geometry_properties: solid " + std::to_string(solid_idx) +
			                         " has no non-empty shell");
		}

		uint32_t cursor = face_start;
		for (auto count : shell_counts) {
			if (count == 0) {
				continue; // dropped shell — contributes no faces and no shell entry
			}
			cursor += count;
			total_shells++;
			regrouped.shell_face_offsets.push_back(cursor);
		}
		regrouped.solid_shell_offsets.push_back(total_shells);
	}

	return BuildSolidModelFromArrowNative(regrouped, vertices);
}

GeomModel BuildGeomModelFromArrowNative(const ArrowNativeBoundaries &boundaries,
                                        const std::vector<Vertex3D> &vertices) {
	// Padding-dimension invariant (design doc): a surface value is a Solid
	// value with solid-count and shell-count both padded to 1 — asserted, not
	// branched on: callers commit to the surface family before calling this
	// function (by calling ST_Geom3DFromArrowNative, not
	// ST_3DFromArrowNative).
	if (boundaries.solid_shell_offsets.size() != 2) {
		throw std::runtime_error("arrow-native geometry: expected a padded (solid-count 1) surface-type value — "
		                         "if this is a real multi-solid value, call BuildSolidModelFromArrowNative instead");
	}
	if (boundaries.shell_face_offsets.size() != 2) {
		throw std::runtime_error("arrow-native geometry: expected shell-count 1 (padding dimension)");
	}

	GeomModel model;
	model.type = GeomType::MultiPolygon;

	uint32_t face_start = boundaries.shell_face_offsets[0];
	uint32_t face_end = boundaries.shell_face_offsets[1];

	model.part_offsets.push_back(0);
	uint32_t total_rings = 0;

	// GeomModel is NOT index-based (unlike SolidModel above) — ring indices
	// are dereferenced and expanded into inline coordinates here, not copied.
	for (uint32_t face_idx = face_start; face_idx < face_end; face_idx++) {
		uint32_t ring_start = boundaries.face_ring_offsets[face_idx];
		uint32_t ring_end = boundaries.face_ring_offsets[face_idx + 1];

		for (uint32_t ring_idx = ring_start; ring_idx < ring_end; ring_idx++) {
			uint32_t idx_start = boundaries.ring_vertex_offsets[ring_idx];
			uint32_t idx_end = boundaries.ring_vertex_offsets[ring_idx + 1];

			model.ring_offsets.push_back(static_cast<uint32_t>(model.vertices.size()));
			for (uint32_t k = idx_start; k < idx_end; k++) {
				uint32_t raw = boundaries.ring_vertex_indices[k];
				if (raw >= vertices.size()) {
					throw std::runtime_error("arrow-native geometry: vertex-pool index out of range");
				}
				model.vertices.push_back(vertices[raw]);
			}
			total_rings++;
		}
		model.part_offsets.push_back(total_rings);
	}
	model.ring_offsets.push_back(static_cast<uint32_t>(model.vertices.size()));

	model.ComputeBBox();
	return model;
}

} // namespace duckdb_3d
