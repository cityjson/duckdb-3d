#include "kernel/crs_transform.hpp"
#include <proj.h>
#include <stdexcept>

// PROJ is confined to this translation unit. The header exposes only opaque
// handles, so the rest of the kernel never sees `proj.h`.

namespace duckdb_3d {

std::string EpsgToAuthString(int32_t srid) {
	if (srid <= 0) {
		throw std::runtime_error("ST_3DTransform: invalid SRID " + std::to_string(srid));
	}
	return "EPSG:" + std::to_string(srid);
}

CrsTransform::CrsTransform(const std::string &source_crs, const std::string &target_crs) {
	PJ_CONTEXT *ctx = proj_context_create();
	if (ctx == nullptr) {
		throw std::runtime_error("ST_3DTransform: failed to create PROJ context");
	}

	PJ *pj = proj_create_crs_to_crs(ctx, source_crs.c_str(), target_crs.c_str(), nullptr);
	if (pj == nullptr) {
		std::string msg = "ST_3DTransform: cannot build transform from '" + source_crs + "' to '" + target_crs + "'";
		proj_context_destroy(ctx);
		throw std::runtime_error(msg);
	}

	// Normalise to easting/northing (lon/lat) so authority axis order (e.g.
	// EPSG:4326 lat/lon) does not surprise callers — matches GIS convention.
	PJ *pj_norm = proj_normalize_for_visualization(ctx, pj);
	proj_destroy(pj);
	if (pj_norm == nullptr) {
		proj_context_destroy(ctx);
		throw std::runtime_error("ST_3DTransform: failed to normalise axis order for '" + source_crs + "' -> '" +
		                         target_crs + "'");
	}

	ctx_ = ctx;
	pj_ = pj_norm;
}

CrsTransform::~CrsTransform() {
	if (pj_ != nullptr) {
		proj_destroy(static_cast<PJ *>(pj_));
	}
	if (ctx_ != nullptr) {
		proj_context_destroy(static_cast<PJ_CONTEXT *>(ctx_));
	}
}

void CrsTransform::ReprojectXY(std::vector<Vertex3D> &vertices) const {
	auto *pj = static_cast<PJ *>(pj_);
	auto *ctx = static_cast<PJ_CONTEXT *>(ctx_);

	for (auto &v : vertices) {
		proj_errno_reset(pj);
		// Feed only X/Y (Z=0); the original Z is reattached below. This keeps the
		// transform strictly horizontal even if the CRS pair implies a 3D pipeline.
		PJ_COORD in = proj_coord(v.x, v.y, 0.0, 0.0);
		PJ_COORD out = proj_trans(pj, PJ_FWD, in);

		int err = proj_errno(pj);
		if (err != 0) {
			throw std::runtime_error(std::string("ST_3DTransform: PROJ transform failed: ") +
			                         proj_context_errno_string(ctx, err));
		}

		v.x = out.xy.x;
		v.y = out.xy.y;
		// v.z is deliberately left unchanged (no vertical datum).
	}
}

} // namespace duckdb_3d
