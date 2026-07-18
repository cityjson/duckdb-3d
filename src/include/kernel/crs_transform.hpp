#pragma once

#include "kernel/solid_model.hpp" // Vertex3D
#include <cstdint>
#include <string>
#include <vector>

namespace duckdb_3d {

//! Format an EPSG integer code as a PROJ authority string, e.g. 28992 -> "EPSG:28992".
//! Throws std::runtime_error for srid <= 0.
std::string EpsgToAuthString(int32_t srid);

//! A reusable horizontal (2D) coordinate transform between two CRS.
//!
//! Wraps a PROJ context and a transform whose axis order is normalised to
//! easting/northing (lon/lat) — matching the GIS/PostGIS convention rather than
//! an authority's declared axis order. Reprojection touches X/Y only; Z is
//! preserved unchanged (no vertical datum). PROJ types never appear in this
//! header: the handles are opaque so `proj.h` stays confined to the .cpp.
class CrsTransform {
public:
	//! Build a transform from `source_crs` to `target_crs`. Each argument must
	//! describe a CRS: an authority code ("EPSG:28992") or a WKT2 CRS string.
	//! Coordinate-operation pipeline strings ("+proj=pipeline ...") are NOT
	//! accepted — PROJ requires a CRS in each position. Throws
	//! std::runtime_error if either CRS is invalid or the transform cannot be
	//! constructed.
	CrsTransform(const std::string &source_crs, const std::string &target_crs);
	~CrsTransform();

	CrsTransform(const CrsTransform &) = delete;
	CrsTransform &operator=(const CrsTransform &) = delete;

	//! Reproject the X/Y of every vertex in place; Z is left untouched.
	//! Throws std::runtime_error if PROJ reports a per-point transform error.
	void ReprojectXY(std::vector<Vertex3D> &vertices) const;

private:
	void *ctx_ = nullptr; //! PJ_CONTEXT*
	void *pj_ = nullptr;  //! PJ* (axis-normalised)
};

} // namespace duckdb_3d
