#pragma once

#include "kernel/geom_model.hpp"
#include "kernel/solid_model.hpp"

#include <cstdint>
#include <vector>

namespace duckdb_3d {

//! What one face measures. A face is a solid's face, or one polygon of a
//! GEOM_3D surface.
struct FaceMeasure {
	//! False when the triangulation could not tile the face (a ring that crosses
	//! itself, a hole outside the exterior): it has no area to report.
	bool has_area = true;
	//! For a solid's face half the length of its Newell vector, the term
	//! ComputeSurfaceArea sums; for a polygon the area of its triangles, the term
	//! Geom3DArea sums. 0 for a face without area.
	double area = 0.0;
	//! False for a face without area, or one that could not be tiled.
	bool has_normal = false;
	//! Unit normal along the face's Newell vector, so it follows the right-hand
	//! rule around the exterior ring: out of a valid solid.
	Vertex3D normal = {0, 0, 0};
};

//! One entry per face of the solid, in storage order.
std::vector<FaceMeasure> SolidFaceMeasures(const SolidModel &model);

//! One entry per polygon of a Polygon, MultiPolygon or PolyhedralSurface;
//! empty for the other classes. A model without its triangulation cache is
//! triangulated first, which throws as TriangulateGeomModel does.
std::vector<FaceMeasure> GeomFaceMeasures(const GeomModel &model);

//! The face's tilt from horizontal in degrees, 0 for a horizontal face and 90
//! for a vertical one, whichever way it faces.
double FaceSlopeDegrees(const Vertex3D &unit_normal);

//! The compass direction the normal leans toward, in degrees clockwise from +Y
//! in [0, 360). False, leaving `out` untouched, when the normal is exactly
//! vertical and leans nowhere.
bool FaceAzimuthDegrees(const Vertex3D &unit_normal, double &out);

//! Face `face` of a solid as a Polygon Z model carrying the solid's CRS, its
//! rings in order with no triangulation cache.
GeomModel SolidFacePolygon(const SolidModel &model, uint32_t face);

//! Shell `shell` of a solid (an index over every shell of the model) as a
//! PolyhedralSurface Z model carrying the solid's CRS, one patch per face.
GeomModel SolidShellSurface(const SolidModel &model, uint32_t shell);

//! Solid `solid` of a model as a model of its own: its shells and faces, the
//! vertices they use, the CRS, and a triangulation and validation of its own.
SolidModel SolidPart(const SolidModel &model, uint32_t solid);

//! Element `n` (from 0) of a GEOM_3D: a part of a MultiPoint, MultiLineString
//! or MultiPolygon, a patch of a PolyhedralSurface, or for the other classes the
//! geometry itself as element 0. False, leaving `out` untouched, when there is
//! no element `n`.
bool GeomElement(const GeomModel &model, uint32_t n, GeomModel &out);

} // namespace duckdb_3d
