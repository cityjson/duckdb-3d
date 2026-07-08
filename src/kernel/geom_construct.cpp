#include "kernel/geom_construct.hpp"

#include "kernel/model_builder.hpp"
#include "kernel/wkb_parser.hpp"

#include <algorithm>
#include <stdexcept>

namespace duckdb_3d {

namespace {

//! Signed XY area of a ring via the shoelace formula. Positive ⇒ CCW.
double SignedAreaXY(const std::vector<Vertex3D> &ring) {
	double area = 0.0;
	size_t n = ring.size();
	for (size_t i = 0; i < n; i++) {
		const auto &a = ring[i];
		const auto &b = ring[(i + 1) % n];
		area += a.x * b.y - b.x * a.y;
	}
	return 0.5 * area;
}

} // namespace

SolidModel BuildSolidFromSurface(const GeomModel &surface) {
	if (surface.type != GeomType::PolyhedralSurface) {
		throw std::runtime_error("ST_MakeSolid: input geometry must be a PolyhedralSurface");
	}
	if (surface.part_offsets.size() < 2) {
		throw std::runtime_error("ST_MakeSolid: surface has no faces");
	}

	// Re-pack the GEOM_3D face/ring topology into a single ParsedPolyhedralSurface.
	ParsedPolyhedralSurface parsed;
	for (size_t k = 0; k + 1 < surface.part_offsets.size(); k++) {
		uint32_t ring_begin = surface.part_offsets[k];
		uint32_t ring_end = surface.part_offsets[k + 1];
		parsed.polygon_ring_counts.push_back(ring_end - ring_begin);
		parsed.polygon_count++;
		for (uint32_t r = ring_begin; r < ring_end; r++) {
			uint32_t v_begin = surface.ring_offsets[r];
			uint32_t v_end = surface.ring_offsets[r + 1];
			parsed.ring_vertex_counts.push_back(v_end - v_begin);
			parsed.vertices.insert(parsed.vertices.end(), surface.vertices.begin() + v_begin,
			                       surface.vertices.begin() + v_end);
		}
	}

	std::vector<ParsedPolyhedralSurface> surfaces{parsed};
	SolidModel model = BuildSolidModel(surfaces);
	if (!(model.validation.is_closed && model.validation.is_manifold &&
	      model.validation.is_oriented)) {
		throw std::runtime_error(
		    "ST_MakeSolid: surface is not a closed, manifold, oriented solid (no repair performed)");
	}
	return model;
}

SolidModel BuildExtrudedSolid(const GeomModel &polygon, double height) {
	if (polygon.type != GeomType::Polygon) {
		throw std::runtime_error("ST_3DExtrude: input geometry must be a Polygon");
	}
	if (height <= 0.0) {
		throw std::runtime_error("ST_3DExtrude: height must be positive");
	}
	if (polygon.ring_offsets.size() < 2) {
		throw std::runtime_error("ST_3DExtrude: polygon has no exterior ring");
	}

	// Exterior ring vertices (ring 0).
	std::vector<Vertex3D> ext(polygon.vertices.begin() + polygon.ring_offsets[0],
	                          polygon.vertices.begin() + polygon.ring_offsets[1]);
	if (ext.size() < 3) {
		throw std::runtime_error("ST_3DExtrude: exterior ring needs at least three vertices");
	}

	// Normalise to CCW so the orientation scheme below yields outward normals.
	if (SignedAreaXY(ext) < 0.0) {
		std::reverse(ext.begin(), ext.end());
	}

	size_t n = ext.size();
	std::vector<Vertex3D> top;
	top.reserve(n);
	for (const auto &v : ext) {
		top.push_back({v.x, v.y, v.z + height});
	}

	// Assemble faces as a single PolyhedralSurface (one ring each), then reuse the
	// canonical construction pipeline (dedup, triangulate, validate).
	ParsedPolyhedralSurface surface;
	auto add_face = [&](const std::vector<Vertex3D> &ring) {
		surface.polygon_ring_counts.push_back(1);
		surface.ring_vertex_counts.push_back(static_cast<uint32_t>(ring.size()));
		surface.vertices.insert(surface.vertices.end(), ring.begin(), ring.end());
		surface.polygon_count++;
	};

	// Bottom face: reversed exterior ring ⇒ downward (outward) normal.
	std::vector<Vertex3D> bottom(ext.rbegin(), ext.rend());
	add_face(bottom);
	// Top face: exterior ring order, translated ⇒ upward (outward) normal.
	add_face(top);
	// Side faces: [ext_i, ext_{i+1}, top_{i+1}, top_i] ⇒ outward normal for CCW ring.
	for (size_t i = 0; i < n; i++) {
		size_t j = (i + 1) % n;
		add_face({ext[i], ext[j], top[j], top[i]});
	}

	std::vector<ParsedPolyhedralSurface> surfaces{surface};
	SolidModel model = BuildSolidModel(surfaces);
	if (!(model.validation.is_closed && model.validation.is_oriented)) {
		throw std::runtime_error("ST_3DExtrude: constructed solid is not closed and oriented");
	}
	return model;
}

} // namespace duckdb_3d
