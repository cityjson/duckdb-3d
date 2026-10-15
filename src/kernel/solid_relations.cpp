#include "kernel/solid_relations.hpp"
#include "kernel/geom_distance.hpp"
#include "kernel/geom_elements.hpp"
#include "kernel/predicates.hpp"

#include <algorithm>
#include <stdexcept>
#include <vector>

namespace duckdb_3d {

namespace {

void RequireValid(const SolidModel &solid) {
	if (!solid.validation.is_valid) {
		throw std::runtime_error("the solid is not valid (ST_3DValidationReport says why), so it bounds no volume");
	}
}

int Sign(double v) {
	return (v > 0.0) - (v < 0.0);
}

//! Sign of orient2d(u, v, p') in the XY plane, where p' is p moved by
//! (eps, eps^2) for an infinitesimal eps: the exact sign when it is not zero,
//! else the sign of the perturbation's leading term. The rule depends only on
//! the directed edge, and reversing the edge negates it, so the triangles that
//! share an edge classify p' consistently and p' is never on an edge.
int PerturbedSide(const Vertex3D &u, const Vertex3D &v, const Vertex3D &p) {
	int s = Orient2D(u, v, p, 2);
	if (s != 0) {
		return s;
	}
	// d/d eps of (v.x - u.x) (p'.y - u.y) - (v.y - u.y) (p'.x - u.x) is
	// -(v.y - u.y); the eps^2 term is (v.x - u.x).
	int t = Sign(u.y - v.y);
	return t != 0 ? t : Sign(v.x - u.x);
}

//! The signed count of the shell triangles of solid `solid_index` that the
//! upward vertical ray from p' crosses: +1 for a triangle facing up, -1 for one
//! facing down. Over a closed, consistently wound boundary that is the solid's
//! winding number about p: 0 outside and in a cavity, +-1 in the material.
int SolidWinding(const SolidModel &m, uint32_t solid_index, const Vertex3D &p) {
	int winding = 0;
	for (uint32_t shell = m.solid_shell_offsets[solid_index]; shell < m.solid_shell_offsets[solid_index + 1]; shell++) {
		uint32_t t_begin = m.face_triangle_offsets[m.shell_face_offsets[shell]];
		uint32_t t_end = m.face_triangle_offsets[m.shell_face_offsets[shell + 1]];
		for (uint32_t t = t_begin; t < t_end; t++) {
			const auto *tri = &m.triangle_vertex_indices[static_cast<size_t>(t) * 3];
			const auto &a = m.vertices[tri[0]];
			const auto &b = m.vertices[tri[1]];
			const auto &c = m.vertices[tri[2]];
			if (p.x < std::min({a.x, b.x, c.x}) || p.x > std::max({a.x, b.x, c.x}) || p.y < std::min({a.y, b.y, c.y}) ||
			    p.y > std::max({a.y, b.y, c.y})) {
				continue;
			}
			// A vertical triangle has no area under the ray, and p' is never on it.
			int facing = Orient2D(a, b, c, 2);
			if (facing == 0) {
				continue;
			}
			if (PerturbedSide(a, b, p) != facing || PerturbedSide(b, c, p) != facing ||
			    PerturbedSide(c, a, p) != facing) {
				continue;
			}
			// Above p: p lies on the side the triangle's normal does not point to
			// when it faces up, and on that side when it faces down. Zero means p
			// is on the triangle, which the callers rule out first.
			if (Orient3D(a, b, c, p) * facing < 0) {
				winding += facing;
			}
		}
	}
	return winding;
}

bool InBox(const Vertex3D &p, const BBox3D &b) {
	return b.min_x <= p.x && p.x <= b.max_x && b.min_y <= p.y && p.y <= b.max_y && b.min_z <= p.z && p.z <= b.max_z;
}

//! One point of every connected part of a geometry: every point of a point
//! set, the first vertex of each line, the first vertex of each polygon. A part
//! that does not meet a solid's surface lies wholly inside its volume or wholly
//! outside it, so this point decides for the part.
std::vector<Vertex3D> PartPoints(const GeomModel &g) {
	std::vector<Vertex3D> out;
	switch (g.type) {
	case GeomType::LineString:
		if (!g.vertices.empty()) {
			out.push_back(g.vertices[0]);
		}
		break;
	case GeomType::MultiLineString:
		for (size_t k = 0; k + 1 < g.part_offsets.size(); k++) {
			if (g.part_offsets[k] < g.part_offsets[k + 1]) {
				out.push_back(g.vertices[g.part_offsets[k]]);
			}
		}
		break;
	case GeomType::Polygon:
	case GeomType::MultiPolygon:
	case GeomType::PolyhedralSurface:
		for (uint32_t polygon = 0; polygon < g.PolygonCount(); polygon++) {
			uint32_t first_ring, ring_count;
			g.PolygonRings(polygon, first_ring, ring_count);
			if (ring_count > 0 && g.ring_offsets[first_ring] < g.ring_offsets[first_ring + 1]) {
				out.push_back(g.vertices[g.ring_offsets[first_ring]]);
			}
		}
		break;
	default:
		out = g.vertices;
		break;
	}
	return out;
}

//! One vertex of every shell: a valid solid's shells are edge-connected.
std::vector<Vertex3D> PartPoints(const SolidModel &s) {
	std::vector<Vertex3D> out;
	for (uint32_t shell = 0; shell < s.ShellCount(); shell++) {
		uint32_t face = s.shell_face_offsets[shell];
		if (face == s.shell_face_offsets[shell + 1]) {
			continue;
		}
		uint32_t ring = s.face_ring_offsets[face];
		out.push_back(s.vertices[s.ring_vertex_indices[s.ring_vertex_offsets[ring]]]);
	}
	return out;
}

bool AnyInside(const SolidModel &solid, const std::vector<Vertex3D> &points) {
	for (const auto &p : points) {
		if (InBox(p, solid.bbox) && PointInSolidVolume(solid, p)) {
			return true;
		}
	}
	return false;
}

} // namespace

bool PointInSolidVolume(const SolidModel &solid, const Vertex3D &p) {
	for (uint32_t s = 0; s < solid.SolidCount(); s++) {
		if (SolidWinding(solid, s, p) != 0) {
			return true;
		}
	}
	return false;
}

double Solid3DDistance(const SolidModel &solid, const GeomModel &geom) {
	RequireValid(solid);
	double d = ElementsDistance(Decompose(solid), solid.bbox, Decompose(geom), geom.bbox);
	if (d == 0.0 || AnyInside(solid, PartPoints(geom))) {
		return 0.0;
	}
	return d;
}

double Solid3DDistance(const SolidModel &a, const SolidModel &b) {
	RequireValid(a);
	RequireValid(b);
	double d = ElementsDistance(Decompose(a), a.bbox, Decompose(b), b.bbox);
	if (d == 0.0 || AnyInside(a, PartPoints(b)) || AnyInside(b, PartPoints(a))) {
		return 0.0;
	}
	return d;
}

bool Solid3DWithin(const SolidModel &solid, const GeomModel &geom, double threshold) {
	RequireValid(solid);
	if (BBoxDistance(solid.bbox, geom.bbox) > threshold) {
		return false;
	}
	return ElementsWithin(Decompose(solid), solid.bbox, Decompose(geom), geom.bbox, threshold) ||
	       AnyInside(solid, PartPoints(geom));
}

bool Solid3DWithin(const SolidModel &a, const SolidModel &b, double threshold) {
	RequireValid(a);
	RequireValid(b);
	if (BBoxDistance(a.bbox, b.bbox) > threshold) {
		return false;
	}
	return ElementsWithin(Decompose(a), a.bbox, Decompose(b), b.bbox, threshold) || AnyInside(a, PartPoints(b)) ||
	       AnyInside(b, PartPoints(a));
}

bool Solid3DIntersects(const SolidModel &solid, const GeomModel &geom) {
	RequireValid(solid);
	if (!BoxesOverlap(solid.bbox, geom.bbox)) {
		return false;
	}
	// Once the surfaces are known not to meet, no part point lies on the solid's
	// surface, so the containment test is exact too.
	return ElementsIntersect(Decompose(solid), solid.bbox, Decompose(geom), geom.bbox) ||
	       AnyInside(solid, PartPoints(geom));
}

bool Solid3DIntersects(const SolidModel &a, const SolidModel &b) {
	RequireValid(a);
	RequireValid(b);
	if (!BoxesOverlap(a.bbox, b.bbox)) {
		return false;
	}
	return ElementsIntersect(Decompose(a), a.bbox, Decompose(b), b.bbox) || AnyInside(a, PartPoints(b)) ||
	       AnyInside(b, PartPoints(a));
}

} // namespace duckdb_3d
