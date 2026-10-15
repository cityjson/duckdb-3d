#include "kernel/faces.hpp"
#include "kernel/geometry_math.hpp"
#include "kernel/triangulation.hpp"
#include "kernel/validation.hpp"

#include <cmath>
#include <unordered_map>

namespace duckdb_3d {

namespace {

constexpr double kDegrees = 180.0 / 3.14159265358979323846;

//! Newell vector of a ring given as `n` vertices reached through `at`, each
//! referenced to the first: a horizontal ring then has exactly zero horizontal
//! components at any magnitude (see NewellRingAreaVector).
template <class AT>
Vertex3D RingNewell(uint32_t n, AT at) {
	Vertex3D sum = {0, 0, 0};
	if (n < 3) {
		return sum;
	}
	const Vertex3D o = at(0);
	for (uint32_t i = 0; i < n; i++) {
		const Vertex3D a = at(i);
		const Vertex3D b = at((i + 1) % n);
		const Vertex3D cur = {a.x - o.x, a.y - o.y, a.z - o.z};
		const Vertex3D next = {b.x - o.x, b.y - o.y, b.z - o.z};
		sum.x += (cur.y - next.y) * (cur.z + next.z);
		sum.y += (cur.z - next.z) * (cur.x + next.x);
		sum.z += (cur.x - next.x) * (cur.y + next.y);
	}
	return sum;
}

double Length(const Vertex3D &v) {
	return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
}

//! Fill `face`'s normal from its Newell vector, when the face has triangles to
//! say it has an interior.
void SetNormal(FaceMeasure &face, const Vertex3D &newell, bool has_triangles) {
	double length = Length(newell);
	if (!has_triangles || length == 0.0) {
		return;
	}
	face.has_normal = true;
	face.normal = {newell.x / length, newell.y / length, newell.z / length};
}

bool IsSurface(GeomType type) {
	return type == GeomType::Polygon || type == GeomType::MultiPolygon || type == GeomType::PolyhedralSurface;
}

} // namespace

std::vector<FaceMeasure> SolidFaceMeasures(const SolidModel &model) {
	std::vector<FaceMeasure> faces(model.FaceCount());
	for (uint32_t f = 0; f < faces.size(); f++) {
		Vertex3D newell = {0, 0, 0};
		for (uint32_t r = model.face_ring_offsets[f]; r < model.face_ring_offsets[f + 1]; r++) {
			auto ring = NewellRingAreaVector(model, r);
			newell.x += ring.x;
			newell.y += ring.y;
			newell.z += ring.z;
		}
		bool has_triangles = model.face_triangle_offsets.size() == faces.size() + 1 &&
		                     model.face_triangle_offsets[f + 1] > model.face_triangle_offsets[f];
		auto &face = faces[f];
		face.area = 0.5 * Length(newell);
		// A face the triangulation could not tile has rings, but no interior that
		// its Newell vector would measure.
		if (!has_triangles && face.area != 0.0) {
			face.has_area = false;
			face.area = 0.0;
			continue;
		}
		SetNormal(face, newell, has_triangles);
	}
	return faces;
}

std::vector<FaceMeasure> GeomFaceMeasures(const GeomModel &geom) {
	if (!IsSurface(geom.type)) {
		return {};
	}
	// A model built in the kernel may not carry its triangles yet.
	const GeomModel *m = &geom;
	GeomModel triangulated;
	if (geom.polygon_triangle_offsets.size() != geom.PolygonCount() + 1) {
		triangulated = geom;
		TriangulateGeomModel(triangulated);
		m = &triangulated;
	}
	const GeomModel &model = *m;
	const uint32_t count = model.PolygonCount();
	std::vector<FaceMeasure> faces(count);
	for (uint32_t p = 0; p < count; p++) {
		uint32_t first_ring = 0;
		uint32_t ring_count = 0;
		model.PolygonRings(p, first_ring, ring_count);
		Vertex3D newell = {0, 0, 0};
		for (uint32_t r = first_ring; r < first_ring + ring_count; r++) {
			const uint32_t begin = model.ring_offsets[r];
			auto ring =
			    RingNewell(model.ring_offsets[r + 1] - begin, [&](uint32_t i) { return model.vertices[begin + i]; });
			// Holes may be wound either way in WKB; on a planar polygon either way
			// keeps the exterior's direction, and only the direction is used.
			newell.x += ring.x;
			newell.y += ring.y;
			newell.z += ring.z;
		}
		auto &face = faces[p];
		const uint32_t t_begin = model.polygon_triangle_offsets[p];
		const uint32_t t_end = model.polygon_triangle_offsets[p + 1];
		for (uint32_t t = t_begin; t < t_end; t++) {
			const auto &a = model.vertices[model.triangle_indices[t * 3]];
			const auto &b = model.vertices[model.triangle_indices[t * 3 + 1]];
			const auto &c = model.vertices[model.triangle_indices[t * 3 + 2]];
			const Vertex3D ab = {b.x - a.x, b.y - a.y, b.z - a.z};
			const Vertex3D ac = {c.x - a.x, c.y - a.y, c.z - a.z};
			face.area +=
			    0.5 * Length({ab.y * ac.z - ab.z * ac.y, ab.z * ac.x - ab.x * ac.z, ab.x * ac.y - ab.y * ac.x});
		}
		SetNormal(face, newell, t_end > t_begin);
	}
	return faces;
}

double FaceSlopeDegrees(const Vertex3D &unit_normal) {
	return std::atan2(std::hypot(unit_normal.x, unit_normal.y), std::fabs(unit_normal.z)) * kDegrees;
}

bool FaceAzimuthDegrees(const Vertex3D &unit_normal, double &out) {
	if (unit_normal.x == 0.0 && unit_normal.y == 0.0) {
		return false;
	}
	double azimuth = std::atan2(unit_normal.x, unit_normal.y) * kDegrees;
	if (azimuth < 0.0) {
		azimuth += 360.0;
		// A hair west of north rounds up to 360, which is north.
		if (azimuth >= 360.0) {
			azimuth = 0.0;
		}
	}
	out = azimuth;
	return true;
}

GeomModel SolidFacePolygon(const SolidModel &model, uint32_t face) {
	GeomModel polygon;
	polygon.type = GeomType::Polygon;
	polygon.crs = model.crs;
	polygon.ring_offsets.push_back(0);
	for (uint32_t r = model.face_ring_offsets[face]; r < model.face_ring_offsets[face + 1]; r++) {
		for (uint32_t i = model.ring_vertex_offsets[r]; i < model.ring_vertex_offsets[r + 1]; i++) {
			polygon.vertices.push_back(model.vertices[model.ring_vertex_indices[i]]);
		}
		polygon.ring_offsets.push_back(static_cast<uint32_t>(polygon.vertices.size()));
	}
	polygon.ComputeBBox();
	return polygon;
}

GeomModel SolidShellSurface(const SolidModel &model, uint32_t shell) {
	GeomModel surface;
	surface.type = GeomType::PolyhedralSurface;
	surface.crs = model.crs;
	surface.ring_offsets.push_back(0);
	surface.part_offsets.push_back(0);
	for (uint32_t f = model.shell_face_offsets[shell]; f < model.shell_face_offsets[shell + 1]; f++) {
		for (uint32_t r = model.face_ring_offsets[f]; r < model.face_ring_offsets[f + 1]; r++) {
			for (uint32_t i = model.ring_vertex_offsets[r]; i < model.ring_vertex_offsets[r + 1]; i++) {
				surface.vertices.push_back(model.vertices[model.ring_vertex_indices[i]]);
			}
			surface.ring_offsets.push_back(static_cast<uint32_t>(surface.vertices.size()));
		}
		surface.part_offsets.push_back(static_cast<uint32_t>(surface.ring_offsets.size() - 1));
	}
	surface.ComputeBBox();
	return surface;
}

SolidModel SolidPart(const SolidModel &model, uint32_t solid) {
	SolidModel part;
	part.crs = model.crs;
	part.solid_shell_offsets = {0};
	part.shell_face_offsets = {0};
	part.face_ring_offsets = {0};
	part.ring_vertex_offsets = {0};
	std::unordered_map<uint32_t, uint32_t> renumber;
	for (uint32_t shell = model.solid_shell_offsets[solid]; shell < model.solid_shell_offsets[solid + 1]; shell++) {
		for (uint32_t f = model.shell_face_offsets[shell]; f < model.shell_face_offsets[shell + 1]; f++) {
			for (uint32_t r = model.face_ring_offsets[f]; r < model.face_ring_offsets[f + 1]; r++) {
				for (uint32_t i = model.ring_vertex_offsets[r]; i < model.ring_vertex_offsets[r + 1]; i++) {
					const uint32_t old_index = model.ring_vertex_indices[i];
					auto found = renumber.emplace(old_index, static_cast<uint32_t>(part.vertices.size()));
					if (found.second) {
						part.vertices.push_back(model.vertices[old_index]);
					}
					part.ring_vertex_indices.push_back(found.first->second);
				}
				part.ring_vertex_offsets.push_back(static_cast<uint32_t>(part.ring_vertex_indices.size()));
			}
			part.face_ring_offsets.push_back(static_cast<uint32_t>(part.ring_vertex_offsets.size() - 1));
		}
		part.shell_face_offsets.push_back(static_cast<uint32_t>(part.face_ring_offsets.size() - 1));
	}
	part.solid_shell_offsets.push_back(static_cast<uint32_t>(part.shell_face_offsets.size() - 1));
	part.ComputeBBox();
	TriangulateSolidModel(part);
	ValidateSolidModel(part);
	return part;
}

bool GeomElement(const GeomModel &model, uint32_t n, GeomModel &out) {
	GeomModel element;
	element.crs = model.crs;
	switch (model.type) {
	case GeomType::MultiPoint:
	case GeomType::MultiLineString: {
		if (n + 1 >= model.part_offsets.size()) {
			return false;
		}
		element.type = model.type == GeomType::MultiPoint ? GeomType::Point : GeomType::LineString;
		element.vertices.assign(model.vertices.begin() + model.part_offsets[n],
		                        model.vertices.begin() + model.part_offsets[n + 1]);
		break;
	}
	case GeomType::MultiPolygon:
	case GeomType::PolyhedralSurface: {
		if (n + 1 >= model.part_offsets.size()) {
			return false;
		}
		element.type = GeomType::Polygon;
		const uint32_t first_ring = model.part_offsets[n];
		const uint32_t end_ring = model.part_offsets[n + 1];
		const uint32_t first_vertex = model.ring_offsets[first_ring];
		element.vertices.assign(model.vertices.begin() + first_vertex,
		                        model.vertices.begin() + model.ring_offsets[end_ring]);
		for (uint32_t r = first_ring; r <= end_ring; r++) {
			element.ring_offsets.push_back(model.ring_offsets[r] - first_vertex);
		}
		break;
	}
	default:
		if (n != 0) {
			return false;
		}
		element = model;
		element.polygon_triangle_offsets.clear();
		element.triangle_indices.clear();
		break;
	}
	element.ComputeBBox();
	out = std::move(element);
	return true;
}

} // namespace duckdb_3d
