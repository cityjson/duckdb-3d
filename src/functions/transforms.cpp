#include "functions/three_d_functions.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/execution/expression_executor.hpp"
#include "duckdb/function/function_set.hpp"
#include "duckdb/function/scalar_function.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"

#include "kernel/affine.hpp"
#include "kernel/crs_transform.hpp"
#include "kernel/validation.hpp"

#include <cmath>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace duckdb {

// Kernel names this file uses unqualified. Using-declarations rather than a
// using-directive, which clang-tidy's google-build-using-namespace rejects.
using duckdb_3d::AffineTransform3D;
using duckdb_3d::CrsTransform;
using duckdb_3d::DeserializeGeomPayload;
using duckdb_3d::DeserializePayload;
using duckdb_3d::EpsgToAuthString;

// ──────────────────────────────────────────────────────────────
// Transforms: ST_3DTranslate(solid SOLID_3D, dx, dy, dz DOUBLE) → SOLID_3D
// ──────────────────────────────────────────────────────────────
static void ST_3DTranslateSolidFun(DataChunk &args, ExpressionState &state, Vector &result) {
	auto count = args.size();

	UnifiedVectorFormat solid_data, dx_data, dy_data, dz_data;
	args.data[0].ToUnifiedFormat(count, solid_data);
	args.data[1].ToUnifiedFormat(count, dx_data);
	args.data[2].ToUnifiedFormat(count, dy_data);
	args.data[3].ToUnifiedFormat(count, dz_data);

	auto solid_strings = UnifiedVectorFormat::GetData<string_t>(solid_data);
	auto dx_vals = UnifiedVectorFormat::GetData<double>(dx_data);
	auto dy_vals = UnifiedVectorFormat::GetData<double>(dy_data);
	auto dz_vals = UnifiedVectorFormat::GetData<double>(dz_data);
	auto &result_validity = FlatVector::Validity(result);

	for (idx_t i = 0; i < count; i++) {
		auto solid_idx = solid_data.sel->get_index(i);
		auto dx_idx = dx_data.sel->get_index(i);
		auto dy_idx = dy_data.sel->get_index(i);
		auto dz_idx = dz_data.sel->get_index(i);

		if (!solid_data.validity.RowIsValid(solid_idx) || !dx_data.validity.RowIsValid(dx_idx) ||
		    !dy_data.validity.RowIsValid(dy_idx) || !dz_data.validity.RowIsValid(dz_idx)) {
			result_validity.SetInvalid(i);
			FlatVector::GetData<string_t>(result)[i] = string_t();
			continue;
		}

		auto &blob = solid_strings[solid_idx];
		auto model = DeserializePayload(reinterpret_cast<const uint8_t *>(blob.GetData()), blob.GetSize());

		double dx = dx_vals[dx_idx], dy = dy_vals[dy_idx], dz = dz_vals[dz_idx];
		// Translation is a rigid motion: shift every vertex and the cached bbox;
		// topology, triangulation indices, and validation flags are unchanged.
		for (auto &v : model.vertices) {
			v.x += dx;
			v.y += dy;
			v.z += dz;
		}
		model.bbox.min_x += dx;
		model.bbox.max_x += dx;
		model.bbox.min_y += dy;
		model.bbox.max_y += dy;
		model.bbox.min_z += dz;
		model.bbox.max_z += dz;

		auto payload = SerializePayload(model);
		FlatVector::GetData<string_t>(result)[i] = StringVector::AddStringOrBlob(
		    result, string_t(reinterpret_cast<const char *>(payload.data()), payload.size()));
	}

	if (args.AllConstant()) {
		result.SetVectorType(VectorType::CONSTANT_VECTOR);
	}
}

// ──────────────────────────────────────────────────────────────
// Transforms: ST_3DTranslate(geom GEOM_3D, dx, dy, dz DOUBLE) → GEOM_3D
// ──────────────────────────────────────────────────────────────
static void ST_3DTranslateGeomFun(DataChunk &args, ExpressionState &state, Vector &result) {
	auto count = args.size();

	UnifiedVectorFormat geom_data, dx_data, dy_data, dz_data;
	args.data[0].ToUnifiedFormat(count, geom_data);
	args.data[1].ToUnifiedFormat(count, dx_data);
	args.data[2].ToUnifiedFormat(count, dy_data);
	args.data[3].ToUnifiedFormat(count, dz_data);

	auto geom_strings = UnifiedVectorFormat::GetData<string_t>(geom_data);
	auto dx_vals = UnifiedVectorFormat::GetData<double>(dx_data);
	auto dy_vals = UnifiedVectorFormat::GetData<double>(dy_data);
	auto dz_vals = UnifiedVectorFormat::GetData<double>(dz_data);
	auto &result_validity = FlatVector::Validity(result);

	for (idx_t i = 0; i < count; i++) {
		auto geom_idx = geom_data.sel->get_index(i);
		auto dx_idx = dx_data.sel->get_index(i);
		auto dy_idx = dy_data.sel->get_index(i);
		auto dz_idx = dz_data.sel->get_index(i);

		if (!geom_data.validity.RowIsValid(geom_idx) || !dx_data.validity.RowIsValid(dx_idx) ||
		    !dy_data.validity.RowIsValid(dy_idx) || !dz_data.validity.RowIsValid(dz_idx)) {
			result_validity.SetInvalid(i);
			FlatVector::GetData<string_t>(result)[i] = string_t();
			continue;
		}

		auto &blob = geom_strings[geom_idx];
		auto model = DeserializeGeomPayload(reinterpret_cast<const uint8_t *>(blob.GetData()), blob.GetSize());

		double dx = dx_vals[dx_idx], dy = dy_vals[dy_idx], dz = dz_vals[dz_idx];
		for (auto &v : model.vertices) {
			v.x += dx;
			v.y += dy;
			v.z += dz;
		}
		model.bbox.min_x += dx;
		model.bbox.max_x += dx;
		model.bbox.min_y += dy;
		model.bbox.max_y += dy;
		model.bbox.min_z += dz;
		model.bbox.max_z += dz;

		auto payload = SerializeGeomPayload(model);
		FlatVector::GetData<string_t>(result)[i] = StringVector::AddStringOrBlob(
		    result, string_t(reinterpret_cast<const char *>(payload.data()), payload.size()));
	}

	if (args.AllConstant()) {
		result.SetVectorType(VectorType::CONSTANT_VECTOR);
	}
}

// ──────────────────────────────────────────────────────────────
// Transforms: ST_3DScale(geom GEOM_3D, sx, sy, sz DOUBLE) → GEOM_3D
// ──────────────────────────────────────────────────────────────
static void ST_3DScaleGeomFun(DataChunk &args, ExpressionState &state, Vector &result) {
	auto count = args.size();

	UnifiedVectorFormat geom_data, sx_data, sy_data, sz_data;
	args.data[0].ToUnifiedFormat(count, geom_data);
	args.data[1].ToUnifiedFormat(count, sx_data);
	args.data[2].ToUnifiedFormat(count, sy_data);
	args.data[3].ToUnifiedFormat(count, sz_data);

	auto geom_strings = UnifiedVectorFormat::GetData<string_t>(geom_data);
	auto sx_vals = UnifiedVectorFormat::GetData<double>(sx_data);
	auto sy_vals = UnifiedVectorFormat::GetData<double>(sy_data);
	auto sz_vals = UnifiedVectorFormat::GetData<double>(sz_data);
	auto &result_validity = FlatVector::Validity(result);

	for (idx_t i = 0; i < count; i++) {
		auto geom_idx = geom_data.sel->get_index(i);
		auto sx_idx = sx_data.sel->get_index(i);
		auto sy_idx = sy_data.sel->get_index(i);
		auto sz_idx = sz_data.sel->get_index(i);

		if (!geom_data.validity.RowIsValid(geom_idx) || !sx_data.validity.RowIsValid(sx_idx) ||
		    !sy_data.validity.RowIsValid(sy_idx) || !sz_data.validity.RowIsValid(sz_idx)) {
			result_validity.SetInvalid(i);
			FlatVector::GetData<string_t>(result)[i] = string_t();
			continue;
		}

		auto &blob = geom_strings[geom_idx];
		auto model = DeserializeGeomPayload(reinterpret_cast<const uint8_t *>(blob.GetData()), blob.GetSize());

		double sx = sx_vals[sx_idx], sy = sy_vals[sy_idx], sz = sz_vals[sz_idx];
		for (auto &v : model.vertices) {
			v.x *= sx;
			v.y *= sy;
			v.z *= sz;
		}
		model.ComputeBBox();

		auto payload = SerializeGeomPayload(model);
		FlatVector::GetData<string_t>(result)[i] = StringVector::AddStringOrBlob(
		    result, string_t(reinterpret_cast<const char *>(payload.data()), payload.size()));
	}

	if (args.AllConstant()) {
		result.SetVectorType(VectorType::CONSTANT_VECTOR);
	}
}

// ──────────────────────────────────────────────────────────────
// Transforms: ST_3DScale(solid SOLID_3D, sx, sy, sz DOUBLE) → SOLID_3D
// ──────────────────────────────────────────────────────────────
static void ST_3DScaleSolidFun(DataChunk &args, ExpressionState &state, Vector &result) {
	auto count = args.size();

	UnifiedVectorFormat solid_data, sx_data, sy_data, sz_data;
	args.data[0].ToUnifiedFormat(count, solid_data);
	args.data[1].ToUnifiedFormat(count, sx_data);
	args.data[2].ToUnifiedFormat(count, sy_data);
	args.data[3].ToUnifiedFormat(count, sz_data);

	auto solid_strings = UnifiedVectorFormat::GetData<string_t>(solid_data);
	auto sx_vals = UnifiedVectorFormat::GetData<double>(sx_data);
	auto sy_vals = UnifiedVectorFormat::GetData<double>(sy_data);
	auto sz_vals = UnifiedVectorFormat::GetData<double>(sz_data);
	auto &result_validity = FlatVector::Validity(result);

	for (idx_t i = 0; i < count; i++) {
		auto solid_idx = solid_data.sel->get_index(i);
		auto sx_idx = sx_data.sel->get_index(i);
		auto sy_idx = sy_data.sel->get_index(i);
		auto sz_idx = sz_data.sel->get_index(i);

		if (!solid_data.validity.RowIsValid(solid_idx) || !sx_data.validity.RowIsValid(sx_idx) ||
		    !sy_data.validity.RowIsValid(sy_idx) || !sz_data.validity.RowIsValid(sz_idx)) {
			result_validity.SetInvalid(i);
			FlatVector::GetData<string_t>(result)[i] = string_t();
			continue;
		}

		auto &blob = solid_strings[solid_idx];
		auto model = DeserializePayload(reinterpret_cast<const uint8_t *>(blob.GetData()), blob.GetSize());

		double sx = sx_vals[sx_idx], sy = sy_vals[sy_idx], sz = sz_vals[sz_idx];
		// Scale about the origin. Unlike a rigid motion, a degenerate (zero) factor
		// collapses faces and a negative factor changes geometry, so the cached
		// validation flags cannot simply carry over. Recompute validation from the
		// scaled coordinates (this catches the collapsed/degenerate case); the bbox
		// is recomputed too since negative factors can swap min/max.
		for (auto &v : model.vertices) {
			v.x *= sx;
			v.y *= sy;
			v.z *= sz;
		}
		model.ComputeBBox();
		ValidateSolidModel(model);

		auto payload = SerializePayload(model);
		FlatVector::GetData<string_t>(result)[i] = StringVector::AddStringOrBlob(
		    result, string_t(reinterpret_cast<const char *>(payload.data()), payload.size()));
	}

	if (args.AllConstant()) {
		result.SetVectorType(VectorType::CONSTANT_VECTOR);
	}
}

// ──────────────────────────────────────────────────────────────
// Transforms: ST_3DRotateX / ST_3DRotateY / ST_3DRotateZ(solid, radians) → SOLID_3D
// ──────────────────────────────────────────────────────────────
enum class RotationAxis { X, Y, Z };

static string_t RotateSolidBlob(Vector &result, string_t solid, double radians, RotationAxis axis) {
	auto model = DeserializePayload(reinterpret_cast<const uint8_t *>(solid.GetData()), solid.GetSize());

	double c = std::cos(radians);
	double s = std::sin(radians);
	// Right-handed, counter-clockwise rotation about the chosen axis (PostGIS
	// convention). A rigid motion: topology, winding, and validation flags are
	// preserved; only the bbox is recomputed.
	for (auto &v : model.vertices) {
		double x = v.x, y = v.y, z = v.z;
		switch (axis) {
		case RotationAxis::X:
			v.y = y * c - z * s;
			v.z = y * s + z * c;
			break;
		case RotationAxis::Y:
			v.x = x * c + z * s;
			v.z = -x * s + z * c;
			break;
		case RotationAxis::Z:
			v.x = x * c - y * s;
			v.y = x * s + y * c;
			break;
		}
	}
	model.ComputeBBox();

	auto payload = SerializePayload(model);
	return StringVector::AddStringOrBlob(result,
	                                     string_t(reinterpret_cast<const char *>(payload.data()), payload.size()));
}

static void ST_3DRotateXSolidFun(DataChunk &args, ExpressionState &state, Vector &result) {
	BinaryExecutor::Execute<string_t, double, string_t>(
	    args.data[0], args.data[1], result, args.size(),
	    [&](string_t solid, double radians) { return RotateSolidBlob(result, solid, radians, RotationAxis::X); });
}

static void ST_3DRotateYSolidFun(DataChunk &args, ExpressionState &state, Vector &result) {
	BinaryExecutor::Execute<string_t, double, string_t>(
	    args.data[0], args.data[1], result, args.size(),
	    [&](string_t solid, double radians) { return RotateSolidBlob(result, solid, radians, RotationAxis::Y); });
}

static void ST_3DRotateZSolidFun(DataChunk &args, ExpressionState &state, Vector &result) {
	BinaryExecutor::Execute<string_t, double, string_t>(
	    args.data[0], args.data[1], result, args.size(),
	    [&](string_t solid, double radians) { return RotateSolidBlob(result, solid, radians, RotationAxis::Z); });
}

// ──────────────────────────────────────────────────────────────
// Transforms: ST_3DRotateX / ST_3DRotateY / ST_3DRotateZ(geom, radians) → GEOM_3D
// ──────────────────────────────────────────────────────────────
static string_t RotateGeomBlob(Vector &result, string_t geom, double radians, RotationAxis axis) {
	auto model = DeserializeGeomPayload(reinterpret_cast<const uint8_t *>(geom.GetData()), geom.GetSize());

	double c = std::cos(radians);
	double s = std::sin(radians);
	for (auto &v : model.vertices) {
		double x = v.x, y = v.y, z = v.z;
		switch (axis) {
		case RotationAxis::X:
			v.y = y * c - z * s;
			v.z = y * s + z * c;
			break;
		case RotationAxis::Y:
			v.x = x * c + z * s;
			v.z = -x * s + z * c;
			break;
		case RotationAxis::Z:
			v.x = x * c - y * s;
			v.y = x * s + y * c;
			break;
		}
	}
	model.ComputeBBox();

	auto payload = SerializeGeomPayload(model);
	return StringVector::AddStringOrBlob(result,
	                                     string_t(reinterpret_cast<const char *>(payload.data()), payload.size()));
}

static void ST_3DRotateXGeomFun(DataChunk &args, ExpressionState &state, Vector &result) {
	BinaryExecutor::Execute<string_t, double, string_t>(
	    args.data[0], args.data[1], result, args.size(),
	    [&](string_t geom, double radians) { return RotateGeomBlob(result, geom, radians, RotationAxis::X); });
}

static void ST_3DRotateYGeomFun(DataChunk &args, ExpressionState &state, Vector &result) {
	BinaryExecutor::Execute<string_t, double, string_t>(
	    args.data[0], args.data[1], result, args.size(),
	    [&](string_t geom, double radians) { return RotateGeomBlob(result, geom, radians, RotationAxis::Y); });
}

static void ST_3DRotateZGeomFun(DataChunk &args, ExpressionState &state, Vector &result) {
	BinaryExecutor::Execute<string_t, double, string_t>(
	    args.data[0], args.data[1], result, args.size(),
	    [&](string_t geom, double radians) { return RotateGeomBlob(result, geom, radians, RotationAxis::Z); });
}

// ──────────────────────────────────────────────────────────────
// Transforms: ST_3DTransform — 2D CRS reprojection (X/Y only, Z preserved).
// Accepts SOLID_3D or GEOM_3D (dispatched by payload magic); output type
// equals input type. PROJ is confined to kernel/crs_transform.
// ──────────────────────────────────────────────────────────────

//! Reproject one payload BLOB using an already-built transform. Recomputes the
//! bbox; re-validates solids because reprojection can invert winding.
static std::vector<uint8_t> ReprojectPayloadBlob(const uint8_t *data, size_t size, const duckdb_3d::CrsTransform &tf) {
	switch (GetPayloadKind(data, size)) {
	case PayloadKind::Solid: {
		auto model = DeserializePayload(data, size);
		tf.ReprojectXY(model.vertices);
		model.ComputeBBox();
		ValidateSolidModel(model);
		return SerializePayload(model);
	}
	case PayloadKind::Geom: {
		auto model = DeserializeGeomPayload(data, size);
		tf.ReprojectXY(model.vertices);
		model.ComputeBBox();
		return SerializeGeomPayload(model);
	}
	default:
		throw InvalidInputException("ST_3DTransform: argument is not a SOLID_3D or GEOM_3D value");
	}
}

//! Per-executor (thread-local) state for ST_3DTransform. Building a PROJ
//! transform is expensive — `proj_create_crs_to_crs` hits the EPSG database on
//! disk — so the cache must outlive a single 2048-row chunk. DuckDB creates one
//! FunctionLocalState per expression executor, i.e. one per query thread, so the
//! map is never shared across threads: each thread owns its own CrsTransform
//! objects, and with them its own PJ_CONTEXT (PROJ contexts are not thread-safe
//! to share). The state dies with the executor at end of query.
struct TransformLocalState : public FunctionLocalState {
	//! One CrsTransform per distinct "source\x1ftarget" pair, reused across chunks.
	std::unordered_map<std::string, std::unique_ptr<duckdb_3d::CrsTransform>> cache;
};

static unique_ptr<FunctionLocalState>
TransformInitLocalState(ExpressionState &state, const BoundFunctionExpression &expr, FunctionData *bind_data) {
	return make_uniq<TransformLocalState>();
}

//! Shared chunk loop. `get_crs(i)` returns the {source, target} CRS strings for
//! row i. One CrsTransform is built per distinct pair and reused across rows —
//! and, via the local state, across every chunk this executor sees.
template <class GetCrs>
static void TransformChunk(DataChunk &args, ExpressionState &state, Vector &result, GetCrs get_crs) {
	auto count = args.size();

	UnifiedVectorFormat geom_data;
	args.data[0].ToUnifiedFormat(count, geom_data);
	auto geom_strings = UnifiedVectorFormat::GetData<string_t>(geom_data);
	auto &result_validity = FlatVector::Validity(result);

	auto &cache = ExecuteFunctionState::GetFunctionState(state)->Cast<TransformLocalState>().cache;

	for (idx_t i = 0; i < count; i++) {
		auto geom_idx = geom_data.sel->get_index(i);

		bool crs_valid = true;
		std::string source;
		std::string target;
		if (!geom_data.validity.RowIsValid(geom_idx) || !get_crs(i, source, target, crs_valid) || !crs_valid) {
			result_validity.SetInvalid(i);
			FlatVector::GetData<string_t>(result)[i] = string_t();
			continue;
		}

		std::string key = source;
		key.push_back('\x1f');
		key += target;
		auto it = cache.find(key);
		if (it == cache.end()) {
			it = cache.emplace(key, std::make_unique<CrsTransform>(source, target)).first;
		}

		auto &blob = geom_strings[geom_idx];
		auto payload =
		    ReprojectPayloadBlob(reinterpret_cast<const uint8_t *>(blob.GetData()), blob.GetSize(), *it->second);
		FlatVector::GetData<string_t>(result)[i] = StringVector::AddStringOrBlob(
		    result, string_t(reinterpret_cast<const char *>(payload.data()), payload.size()));
	}

	if (args.AllConstant()) {
		result.SetVectorType(VectorType::CONSTANT_VECTOR);
	}
}

// ST_3DTransform(geom, source_crs VARCHAR, target_crs VARCHAR)
static void ST_3DTransformStrFun(DataChunk &args, ExpressionState &state, Vector &result) {
	auto count = args.size();
	UnifiedVectorFormat src_data, tgt_data;
	args.data[1].ToUnifiedFormat(count, src_data);
	args.data[2].ToUnifiedFormat(count, tgt_data);
	auto src_strings = UnifiedVectorFormat::GetData<string_t>(src_data);
	auto tgt_strings = UnifiedVectorFormat::GetData<string_t>(tgt_data);

	TransformChunk(args, state, result, [&](idx_t i, std::string &source, std::string &target, bool &valid) -> bool {
		auto si = src_data.sel->get_index(i);
		auto ti = tgt_data.sel->get_index(i);
		if (!src_data.validity.RowIsValid(si) || !tgt_data.validity.RowIsValid(ti)) {
			valid = false;
			return false;
		}
		source = src_strings[si].GetString();
		target = tgt_strings[ti].GetString();
		return true;
	});
}

// ST_3DTransform(geom, source_srid INTEGER, target_srid INTEGER)
static void ST_3DTransformIntFun(DataChunk &args, ExpressionState &state, Vector &result) {
	auto count = args.size();
	UnifiedVectorFormat src_data, tgt_data;
	args.data[1].ToUnifiedFormat(count, src_data);
	args.data[2].ToUnifiedFormat(count, tgt_data);
	auto src_vals = UnifiedVectorFormat::GetData<int32_t>(src_data);
	auto tgt_vals = UnifiedVectorFormat::GetData<int32_t>(tgt_data);

	TransformChunk(args, state, result, [&](idx_t i, std::string &source, std::string &target, bool &valid) -> bool {
		auto si = src_data.sel->get_index(i);
		auto ti = tgt_data.sel->get_index(i);
		if (!src_data.validity.RowIsValid(si) || !tgt_data.validity.RowIsValid(ti)) {
			valid = false;
			return false;
		}
		source = EpsgToAuthString(src_vals[si]);
		target = EpsgToAuthString(tgt_vals[ti]);
		return true;
	});
}

// ──────────────────────────────────────────────────────────────
// ST_3DPlaceImplicit(relative SOLID_3D | GEOM_3D, reference_point GEOM_3D,
//                    transformation_matrix DOUBLE[]) → same type as `relative`
//
// Materialises an implicit geometry: every relative vertex v lands at
// M * v + p, M the row-major 4x4 matrix and p the reference point. A NULL
// matrix is the identity, so the NULL handling is special; a NULL relative
// geometry or reference point gives NULL. The transform itself is kernel math
// (kernel/affine), with no knowledge of where the arguments came from.
// ──────────────────────────────────────────────────────────────

//! The placement transform for one row: the matrix (identity when NULL), then a
//! translation to the reference point.
static AffineTransform3D PlacementFor(Vector &point_vec, const UnifiedVectorFormat &point_data, idx_t point_idx,
                                      Vector &matrix_vec, const UnifiedVectorFormat &matrix_data, idx_t matrix_idx) {
	auto &point_blob = UnifiedVectorFormat::GetData<string_t>(point_data)[point_idx];
	auto point = DeserializeGeomPayload(reinterpret_cast<const uint8_t *>(point_blob.GetData()), point_blob.GetSize());
	if (point.type != duckdb_3d::GeomType::Point || point.vertices.size() != 1) {
		throw InvalidInputException("ST_3DPlaceImplicit: reference point must be a Point");
	}

	AffineTransform3D placement = duckdb_3d::AffineIdentity();
	if (matrix_data.validity.RowIsValid(matrix_idx)) {
		auto entry = UnifiedVectorFormat::GetData<list_entry_t>(matrix_data)[matrix_idx];
		auto &child = ListVector::GetEntry(matrix_vec);
		UnifiedVectorFormat child_data;
		child.ToUnifiedFormat(ListVector::GetListSize(matrix_vec), child_data);
		auto child_values = UnifiedVectorFormat::GetData<double>(child_data);
		std::vector<double> values;
		values.reserve(entry.length);
		for (idx_t k = 0; k < entry.length; k++) {
			auto child_idx = child_data.sel->get_index(entry.offset + k);
			if (!child_data.validity.RowIsValid(child_idx)) {
				throw InvalidInputException("ST_3DPlaceImplicit: transformation matrix contains a NULL value");
			}
			values.push_back(child_values[child_idx]);
		}
		try {
			placement = duckdb_3d::AffineFromMatrix4x4(values);
		} catch (const std::runtime_error &e) {
			throw InvalidInputException(std::string("ST_3DPlaceImplicit: ") + e.what());
		}
	}
	return duckdb_3d::ThenTranslate(placement, point.vertices[0]);
}

template <bool SOLID>
static void ST_3DPlaceImplicitFun(DataChunk &args, ExpressionState &state, Vector &result) {
	auto count = args.size();
	UnifiedVectorFormat geom_data, point_data, matrix_data;
	args.data[0].ToUnifiedFormat(count, geom_data);
	args.data[1].ToUnifiedFormat(count, point_data);
	args.data[2].ToUnifiedFormat(count, matrix_data);
	auto geom_strings = UnifiedVectorFormat::GetData<string_t>(geom_data);
	auto &result_validity = FlatVector::Validity(result);
	auto result_data = FlatVector::GetData<string_t>(result);

	for (idx_t i = 0; i < count; i++) {
		auto geom_idx = geom_data.sel->get_index(i);
		auto point_idx = point_data.sel->get_index(i);
		if (!geom_data.validity.RowIsValid(geom_idx) || !point_data.validity.RowIsValid(point_idx)) {
			result_validity.SetInvalid(i);
			result_data[i] = string_t();
			continue;
		}
		auto placement =
		    PlacementFor(args.data[1], point_data, point_idx, args.data[2], matrix_data, matrix_data.sel->get_index(i));
		auto &blob = geom_strings[geom_idx];
		std::vector<uint8_t> payload;
		if constexpr (SOLID) {
			// A singular linear part maps every solid to zero volume, and the result
			// can still pass every validity check (a projection flattens faces into
			// non-degenerate ones), so it is refused rather than returned.
			if (duckdb_3d::IsSingularLinear(placement)) {
				throw InvalidInputException("ST_3DPlaceImplicit: transformation matrix is singular; it would "
				                            "collapse the solid to zero volume");
			}
			auto model = DeserializePayload(reinterpret_cast<const uint8_t *>(blob.GetData()), blob.GetSize());
			duckdb_3d::ApplyAffine(placement, model.vertices);
			// The triangulation's indices still tile each face under a non-singular
			// affine map; a mirroring matrix reverses handedness, so re-validate.
			model.ComputeBBox();
			ValidateSolidModel(model);
			payload = SerializePayload(model);
		} else {
			auto model = DeserializeGeomPayload(reinterpret_cast<const uint8_t *>(blob.GetData()), blob.GetSize());
			duckdb_3d::ApplyAffine(placement, model.vertices);
			model.ComputeBBox();
			payload = SerializeGeomPayload(model);
		}
		result_data[i] = StringVector::AddStringOrBlob(
		    result, string_t(reinterpret_cast<const char *>(payload.data()), payload.size()));
	}

	if (args.AllConstant()) {
		result.SetVectorType(VectorType::CONSTANT_VECTOR);
	}
}

void RegisterTransformFunctions(ExtensionLoader &loader, const LogicalType &solid_3d_type,
                                const LogicalType &geom_3d_type) {
	// Transform functions
	ScalarFunctionSet translate_set("st_3dtranslate");
	translate_set.AddFunction(
	    ScalarFunction({LogicalType::BLOB, LogicalType::DOUBLE, LogicalType::DOUBLE, LogicalType::DOUBLE},
	                   LogicalType::BLOB, ST_3DTranslateSolidFun));
	translate_set.AddFunction(
	    ScalarFunction({solid_3d_type, LogicalType::DOUBLE, LogicalType::DOUBLE, LogicalType::DOUBLE}, solid_3d_type,
	                   ST_3DTranslateSolidFun));
	translate_set.AddFunction(
	    ScalarFunction({geom_3d_type, LogicalType::DOUBLE, LogicalType::DOUBLE, LogicalType::DOUBLE}, geom_3d_type,
	                   ST_3DTranslateGeomFun));
	RegisterDocumented(loader, std::move(translate_set),
	                   {{"geom", "dx", "dy", "dz"},
	                    "Translates a SOLID_3D or GEOM_3D value by (dx, dy, dz); the result has the input's type.",
	                    "ST_3DTranslate(ST_Geom3DFromWKB('POINT Z (1 2 3)'::GEOMETRY), 10.0, 0.0, -1.0)",
	                    {"transform"}});
	ScalarFunctionSet scale_set("st_3dscale");
	scale_set.AddFunction(
	    ScalarFunction({LogicalType::BLOB, LogicalType::DOUBLE, LogicalType::DOUBLE, LogicalType::DOUBLE},
	                   LogicalType::BLOB, ST_3DScaleSolidFun));
	scale_set.AddFunction(ScalarFunction({solid_3d_type, LogicalType::DOUBLE, LogicalType::DOUBLE, LogicalType::DOUBLE},
	                                     solid_3d_type, ST_3DScaleSolidFun));
	scale_set.AddFunction(ScalarFunction({geom_3d_type, LogicalType::DOUBLE, LogicalType::DOUBLE, LogicalType::DOUBLE},
	                                     geom_3d_type, ST_3DScaleGeomFun));
	RegisterDocumented(
	    loader, std::move(scale_set),
	    {{"geom", "sx", "sy", "sz"},
	     "Scales a SOLID_3D or GEOM_3D value about the origin by (sx, sy, sz); the result has the input's type.",
	     "ST_3DScale(ST_Geom3DFromWKB('POINT Z (1 2 3)'::GEOMETRY), 2.0, 2.0, 2.0)",
	     {"transform"}});
	ScalarFunctionSet rotatex_set("st_3drotatex");
	rotatex_set.AddFunction(
	    ScalarFunction({LogicalType::BLOB, LogicalType::DOUBLE}, LogicalType::BLOB, ST_3DRotateXSolidFun));
	rotatex_set.AddFunction(ScalarFunction({solid_3d_type, LogicalType::DOUBLE}, solid_3d_type, ST_3DRotateXSolidFun));
	rotatex_set.AddFunction(ScalarFunction({geom_3d_type, LogicalType::DOUBLE}, geom_3d_type, ST_3DRotateXGeomFun));
	RegisterDocumented(loader, std::move(rotatex_set),
	                   {{"geom", "radians"},
	                    "Rotates a SOLID_3D or GEOM_3D value about the X axis by an angle in radians, right-handed and "
	                    "counter-clockwise; the result has the input's type.",
	                    "ST_3DRotateX(ST_Geom3DFromWKB('POINT Z (1 2 3)'::GEOMETRY), pi() / 2)",
	                    {"transform"}});

	ScalarFunctionSet rotatey_set("st_3drotatey");
	rotatey_set.AddFunction(
	    ScalarFunction({LogicalType::BLOB, LogicalType::DOUBLE}, LogicalType::BLOB, ST_3DRotateYSolidFun));
	rotatey_set.AddFunction(ScalarFunction({solid_3d_type, LogicalType::DOUBLE}, solid_3d_type, ST_3DRotateYSolidFun));
	rotatey_set.AddFunction(ScalarFunction({geom_3d_type, LogicalType::DOUBLE}, geom_3d_type, ST_3DRotateYGeomFun));
	RegisterDocumented(loader, std::move(rotatey_set),
	                   {{"geom", "radians"},
	                    "Rotates a SOLID_3D or GEOM_3D value about the Y axis by an angle in radians, right-handed and "
	                    "counter-clockwise; the result has the input's type.",
	                    "ST_3DRotateY(ST_Geom3DFromWKB('POINT Z (1 2 3)'::GEOMETRY), pi() / 2)",
	                    {"transform"}});

	ScalarFunctionSet rotatez_set("st_3drotatez");
	rotatez_set.AddFunction(
	    ScalarFunction({LogicalType::BLOB, LogicalType::DOUBLE}, LogicalType::BLOB, ST_3DRotateZSolidFun));
	rotatez_set.AddFunction(ScalarFunction({solid_3d_type, LogicalType::DOUBLE}, solid_3d_type, ST_3DRotateZSolidFun));
	rotatez_set.AddFunction(ScalarFunction({geom_3d_type, LogicalType::DOUBLE}, geom_3d_type, ST_3DRotateZGeomFun));
	RegisterDocumented(loader, std::move(rotatez_set),
	                   {{"geom", "radians"},
	                    "Rotates a SOLID_3D or GEOM_3D value about the Z axis by an angle in radians, right-handed and "
	                    "counter-clockwise; the result has the input's type.",
	                    "ST_3DRotateZ(ST_Geom3DFromWKB('POINT Z (1 2 3)'::GEOMETRY), pi() / 2)",
	                    {"transform"}});

	// ST_3DTransform: 2D CRS reprojection. EPSG-integer and CRS-string forms, each
	// on SOLID_3D and GEOM_3D. Output type equals input type. Every overload gets
	// the same local-state initialiser so the PROJ transform cache survives across
	// chunks (see TransformLocalState).
	ScalarFunctionSet transform_set("st_3dtransform");
	auto add_transform = [&transform_set](ScalarFunction fun) {
		fun.SetInitStateCallback(TransformInitLocalState);
		transform_set.AddFunction(std::move(fun));
	};
	add_transform(ScalarFunction({LogicalType::BLOB, LogicalType::INTEGER, LogicalType::INTEGER}, LogicalType::BLOB,
	                             ST_3DTransformIntFun));
	add_transform(ScalarFunction({LogicalType::BLOB, LogicalType::VARCHAR, LogicalType::VARCHAR}, LogicalType::BLOB,
	                             ST_3DTransformStrFun));
	add_transform(ScalarFunction({solid_3d_type, LogicalType::INTEGER, LogicalType::INTEGER}, solid_3d_type,
	                             ST_3DTransformIntFun));
	add_transform(
	    ScalarFunction({geom_3d_type, LogicalType::INTEGER, LogicalType::INTEGER}, geom_3d_type, ST_3DTransformIntFun));
	add_transform(ScalarFunction({solid_3d_type, LogicalType::VARCHAR, LogicalType::VARCHAR}, solid_3d_type,
	                             ST_3DTransformStrFun));
	add_transform(
	    ScalarFunction({geom_3d_type, LogicalType::VARCHAR, LogicalType::VARCHAR}, geom_3d_type, ST_3DTransformStrFun));
	RegisterDocumented(loader, std::move(transform_set),
	                   {{"geom", "source_crs", "target_crs"},
	                    "Reprojects a SOLID_3D or GEOM_3D value between CRSs given as EPSG integers or CRS strings; X "
	                    "and Y are reprojected and Z passes through unchanged. Solids are re-validated afterwards.",
	                    "ST_3DTransform(ST_Geom3DFromWKB('POINT Z (85000 446800 5)'::GEOMETRY), 28992, 4326)",
	                    {"transform"}});
	// ST_3DPlaceImplicit: an implicit geometry's relative geometry placed by its
	// transformation matrix and reference point. NULL matrix = identity, hence
	// SPECIAL_HANDLING.
	ScalarFunctionSet place_set("st_3dplaceimplicit");
	auto matrix_type = LogicalType::LIST(LogicalType::DOUBLE);
	ScalarFunction place_solid({solid_3d_type, geom_3d_type, matrix_type}, solid_3d_type, ST_3DPlaceImplicitFun<true>);
	place_solid.SetNullHandling(FunctionNullHandling::SPECIAL_HANDLING);
	place_set.AddFunction(place_solid);
	ScalarFunction place_geom({geom_3d_type, geom_3d_type, matrix_type}, geom_3d_type, ST_3DPlaceImplicitFun<false>);
	place_geom.SetNullHandling(FunctionNullHandling::SPECIAL_HANDLING);
	place_set.AddFunction(place_geom);
	RegisterDocumented(
	    loader, std::move(place_set),
	    {{"relative_geometry", "reference_point", "transformation_matrix"},
	     "Places an implicit geometry: maps each vertex v of the relative geometry (SOLID_3D or GEOM_3D) to M * v + p, "
	     "with M the row-major 4x4 transformation matrix (16 values, last row 0 0 0 1; NULL means identity) and p the "
	     "reference point (a GEOM_3D Point). The result has the input's type; solids are re-validated, and a singular "
	     "matrix raises for a solid.",
	     "ST_3DPlaceImplicit(ST_Geom3DFromWKB('POINT Z (1 0 0)'::GEOMETRY), ST_Geom3DFromWKB('POINT Z (10 20 "
	     "30)'::GEOMETRY), "
	     "[2.0, 0, 0, 0, 0, 2, 0, 0, 0, 0, 2, 0, 0, 0, 0, 1])",
	     {"transform"}});
}

} // namespace duckdb
