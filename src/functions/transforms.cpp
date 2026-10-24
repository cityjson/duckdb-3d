#include "functions/three_d_functions.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/vector/vector_iterator.hpp"
#include "duckdb/common/vector/vector_writer.hpp"
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

//! Runs a (payload, x, y, z DOUBLE) → payload transform row by row. A NULL in
//! any argument gives NULL. `transform` maps the payload bytes and the three
//! factors to the new payload.
template <class FUNC>
static void ExecutePayloadXYZ(DataChunk &args, Vector &result, FUNC &&transform) {
	auto count = args.size();
	auto payloads = args.data[0].Values<string_t>();
	auto xs = args.data[1].Values<double>();
	auto ys = args.data[2].Values<double>();
	auto zs = args.data[3].Values<double>();
	auto writer = FlatVector::Writer<string_t>(result, count);
	for (idx_t i = 0; i < count; i++) {
		auto payload = payloads[i];
		auto x = xs[i];
		auto y = ys[i];
		auto z = zs[i];
		if (!payload.IsValid() || !x.IsValid() || !y.IsValid() || !z.IsValid()) {
			writer.WriteNull();
			continue;
		}
		auto &blob = payload.GetValue();
		auto bytes = transform(reinterpret_cast<const uint8_t *>(blob.GetData()), blob.GetSize(), x.GetValue(),
		                       y.GetValue(), z.GetValue());
		writer.WriteValue(string_t(reinterpret_cast<const char *>(bytes.data()), bytes.size()));
	}
}

//! Shift every vertex and the cached bbox of a model by (dx, dy, dz).
template <class MODEL>
static void TranslateModel(MODEL &model, double dx, double dy, double dz) {
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
}

//! Scale every vertex of a model about the origin by (sx, sy, sz).
template <class MODEL>
static void ScaleModel(MODEL &model, double sx, double sy, double sz) {
	for (auto &v : model.vertices) {
		v.x *= sx;
		v.y *= sy;
		v.z *= sz;
	}
	model.ComputeBBox();
}

// ──────────────────────────────────────────────────────────────
// Transforms: ST_3DTranslate(solid SOLID_3D, dx, dy, dz DOUBLE) → SOLID_3D
// ──────────────────────────────────────────────────────────────
static void ST_3DTranslateSolidFun(DataChunk &args, ExpressionState &state, Vector &result) {
	ExecutePayloadXYZ(args, result, [](const uint8_t *data, idx_t size, double dx, double dy, double dz) {
		auto model = DeserializePayload(data, size);
		// Translation is a rigid motion: shift every vertex and the cached bbox;
		// topology, triangulation indices, and validation flags are unchanged.
		TranslateModel(model, dx, dy, dz);
		return SerializePayload(model);
	});
}

// ──────────────────────────────────────────────────────────────
// Transforms: ST_3DTranslate(geom GEOM_3D, dx, dy, dz DOUBLE) → GEOM_3D
// ──────────────────────────────────────────────────────────────
static void ST_3DTranslateGeomFun(DataChunk &args, ExpressionState &state, Vector &result) {
	ExecutePayloadXYZ(args, result, [](const uint8_t *data, idx_t size, double dx, double dy, double dz) {
		auto model = DeserializeGeomPayload(data, size);
		TranslateModel(model, dx, dy, dz);
		return SerializeGeomPayload(model);
	});
}

// ──────────────────────────────────────────────────────────────
// Transforms: ST_3DScale(geom GEOM_3D, sx, sy, sz DOUBLE) → GEOM_3D
// ──────────────────────────────────────────────────────────────
static void ST_3DScaleGeomFun(DataChunk &args, ExpressionState &state, Vector &result) {
	ExecutePayloadXYZ(args, result, [](const uint8_t *data, idx_t size, double sx, double sy, double sz) {
		auto model = DeserializeGeomPayload(data, size);
		ScaleModel(model, sx, sy, sz);
		return SerializeGeomPayload(model);
	});
}

// ──────────────────────────────────────────────────────────────
// Transforms: ST_3DScale(solid SOLID_3D, sx, sy, sz DOUBLE) → SOLID_3D
// ──────────────────────────────────────────────────────────────
static void ST_3DScaleSolidFun(DataChunk &args, ExpressionState &state, Vector &result) {
	ExecutePayloadXYZ(args, result, [](const uint8_t *data, idx_t size, double sx, double sy, double sz) {
		auto model = DeserializePayload(data, size);
		// Scale about the origin. A uniform factor is a similarity: like a rigid
		// motion it keeps every validation verdict, each of which is measured
		// relative to the solid's own extent, and a negative factor reverses every
		// face alike. That holds while every scaled coordinate keeps full
		// precision (no overflow, no underflow into subnormals). Any other factor
		// changes the solid's shape, and a zero one collapses faces, so validation
		// runs again on the scaled coordinates. The bbox is recomputed either way:
		// a negative factor swaps min and max.
		bool similarity = sx == sy && sy == sz && std::isnormal(sx);
		auto full_precision = [](double c) {
			return c == 0.0 || std::isnormal(c);
		};
		for (auto &v : model.vertices) {
			v.x *= sx;
			v.y *= sy;
			v.z *= sz;
			similarity = similarity && full_precision(v.x) && full_precision(v.y) && full_precision(v.z);
		}
		model.ComputeBBox();
		if (!similarity) {
			ValidateSolidModel(model);
		}

		return SerializePayload(model);
	});
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
	    args.data[0], args.data[1], result,
	    [&](string_t solid, double radians) { return RotateSolidBlob(result, solid, radians, RotationAxis::X); });
}

static void ST_3DRotateYSolidFun(DataChunk &args, ExpressionState &state, Vector &result) {
	BinaryExecutor::Execute<string_t, double, string_t>(
	    args.data[0], args.data[1], result,
	    [&](string_t solid, double radians) { return RotateSolidBlob(result, solid, radians, RotationAxis::Y); });
}

static void ST_3DRotateZSolidFun(DataChunk &args, ExpressionState &state, Vector &result) {
	BinaryExecutor::Execute<string_t, double, string_t>(
	    args.data[0], args.data[1], result,
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
	    args.data[0], args.data[1], result,
	    [&](string_t geom, double radians) { return RotateGeomBlob(result, geom, radians, RotationAxis::X); });
}

static void ST_3DRotateYGeomFun(DataChunk &args, ExpressionState &state, Vector &result) {
	BinaryExecutor::Execute<string_t, double, string_t>(
	    args.data[0], args.data[1], result,
	    [&](string_t geom, double radians) { return RotateGeomBlob(result, geom, radians, RotationAxis::Y); });
}

static void ST_3DRotateZGeomFun(DataChunk &args, ExpressionState &state, Vector &result) {
	BinaryExecutor::Execute<string_t, double, string_t>(
	    args.data[0], args.data[1], result,
	    [&](string_t geom, double radians) { return RotateGeomBlob(result, geom, radians, RotationAxis::Z); });
}

// ──────────────────────────────────────────────────────────────
// Transforms: ST_3DTransform — 2D CRS reprojection (X/Y only, Z preserved).
// Accepts SOLID_3D or GEOM_3D (dispatched by payload magic); output type
// equals input type. PROJ is confined to kernel/crs_transform.
// ──────────────────────────────────────────────────────────────

//! Reproject one payload BLOB using an already-built transform. Recomputes the
//! bbox; re-validates solids because reprojection can invert winding.
//! The result is labelled with `target`.
static std::vector<uint8_t> ReprojectPayloadBlob(const uint8_t *data, size_t size, const duckdb_3d::CrsTransform &tf,
                                                 const std::string &target) {
	switch (GetPayloadKind(data, size)) {
	case PayloadKind::Solid: {
		auto model = DeserializePayload(data, size);
		tf.ReprojectXY(model.vertices);
		model.crs = target;
		model.ComputeBBox();
		ValidateSolidModel(model);
		return SerializePayload(model);
	}
	case PayloadKind::Geom: {
		auto model = DeserializeGeomPayload(data, size);
		tf.ReprojectXY(model.vertices);
		model.crs = target;
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
//! row i, the source empty when the value's own CRS is the source. One
//! CrsTransform is built per distinct pair and reused across rows — and, via the
//! local state, across every chunk this executor sees.
template <class GetCrs>
static void TransformChunk(DataChunk &args, ExpressionState &state, Vector &result, GetCrs get_crs) {
	auto count = args.size();
	auto geoms = args.data[0].Values<string_t>();
	auto &cache = ExecuteFunctionState::GetFunctionState(state)->Cast<TransformLocalState>().cache;

	auto writer = FlatVector::Writer<string_t>(result, count);
	for (idx_t i = 0; i < count; i++) {
		auto geom = geoms[i];
		std::string source;
		std::string target;
		if (!geom.IsValid() || !get_crs(i, source, target)) {
			writer.WriteNull();
			continue;
		}

		auto &blob = geom.GetValue();
		source = TransformSourceCrs(PayloadCrs(blob), source);
		std::string key = source;
		key.push_back('\x1f');
		key += target;
		auto it = cache.find(key);
		if (it == cache.end()) {
			it = cache.emplace(key, std::make_unique<CrsTransform>(source, target)).first;
		}

		auto payload = ReprojectPayloadBlob(reinterpret_cast<const uint8_t *>(blob.GetData()), blob.GetSize(),
		                                    *it->second, target);
		writer.WriteValue(string_t(reinterpret_cast<const char *>(payload.data()), payload.size()));
	}
}

// ST_3DTransform(geom, source_crs VARCHAR, target_crs VARCHAR)
static void ST_3DTransformStrFun(DataChunk &args, ExpressionState &state, Vector &result) {
	auto sources = args.data[1].Values<string_t>();
	auto targets = args.data[2].Values<string_t>();
	TransformChunk(args, state, result, [&](idx_t i, std::string &source, std::string &target) {
		auto source_entry = sources[i];
		auto target_entry = targets[i];
		if (!source_entry.IsValid() || !target_entry.IsValid()) {
			return false;
		}
		source = source_entry.GetValue().GetString();
		target = target_entry.GetValue().GetString();
		return true;
	});
}

// ST_3DTransform(geom, source_srid INTEGER, target_srid INTEGER)
static void ST_3DTransformIntFun(DataChunk &args, ExpressionState &state, Vector &result) {
	auto sources = args.data[1].Values<int32_t>();
	auto targets = args.data[2].Values<int32_t>();
	TransformChunk(args, state, result, [&](idx_t i, std::string &source, std::string &target) {
		auto source_entry = sources[i];
		auto target_entry = targets[i];
		if (!source_entry.IsValid() || !target_entry.IsValid()) {
			return false;
		}
		source = EpsgToAuthString(source_entry.GetValue());
		target = EpsgToAuthString(target_entry.GetValue());
		return true;
	});
}

// ST_3DTransform(geom, target_crs VARCHAR): from the value's own CRS
static void ST_3DTransformToStrFun(DataChunk &args, ExpressionState &state, Vector &result) {
	auto targets = args.data[1].Values<string_t>();
	TransformChunk(args, state, result, [&](idx_t i, std::string &source, std::string &target) {
		auto target_entry = targets[i];
		if (!target_entry.IsValid()) {
			return false;
		}
		target = target_entry.GetValue().GetString();
		return true;
	});
}

// ST_3DTransform(geom, target_srid INTEGER): from the value's own CRS
static void ST_3DTransformToIntFun(DataChunk &args, ExpressionState &state, Vector &result) {
	auto targets = args.data[1].Values<int32_t>();
	TransformChunk(args, state, result, [&](idx_t i, std::string &source, std::string &target) {
		auto target_entry = targets[i];
		if (!target_entry.IsValid()) {
			return false;
		}
		target = EpsgToAuthString(target_entry.GetValue());
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
static AffineTransform3D PlacementFor(const string_t &point_blob,
                                      const VectorIterator<VectorListType<double>>::ValueEntry &matrix) {
	auto point = DeserializeGeomPayload(reinterpret_cast<const uint8_t *>(point_blob.GetData()), point_blob.GetSize());
	if (point.type != duckdb_3d::GeomType::Point || point.vertices.size() != 1) {
		throw InvalidInputException("ST_3DPlaceImplicit: reference point must be a Point");
	}

	AffineTransform3D placement = duckdb_3d::AffineIdentity();
	if (matrix.IsValid()) {
		std::vector<double> values;
		values.reserve(matrix.GetListLength());
		for (auto element : matrix.GetChildValues()) {
			if (!element.IsValid()) {
				throw InvalidInputException("ST_3DPlaceImplicit: transformation matrix contains a NULL value");
			}
			values.push_back(element.GetValue());
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
	auto geoms = args.data[0].Values<string_t>();
	auto points = args.data[1].Values<string_t>();
	// The list iterator does not take a dictionary vector.
	if (args.data[2].GetVectorType() == VectorType::DICTIONARY_VECTOR) {
		args.data[2].Flatten();
	}
	auto matrices = args.data[2].Values<VectorListType<double>>();

	auto writer = FlatVector::Writer<string_t>(result, count);
	for (idx_t i = 0; i < count; i++) {
		auto geom = geoms[i];
		auto point = points[i];
		if (!geom.IsValid() || !point.IsValid()) {
			writer.WriteNull();
			continue;
		}
		auto placement = PlacementFor(point.GetValue(), matrices[i]);
		// The placed geometry is in the reference point's CRS; the relative one's
		// coordinates are local.
		auto crs = PayloadCrs(point.GetValue());
		auto &blob = geom.GetValue();
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
			model.crs = crs;
			// The triangulation's indices still tile each face under a non-singular
			// affine map; a mirroring matrix reverses handedness, so re-validate.
			model.ComputeBBox();
			ValidateSolidModel(model);
			payload = SerializePayload(model);
		} else {
			auto model = DeserializeGeomPayload(reinterpret_cast<const uint8_t *>(blob.GetData()), blob.GetSize());
			duckdb_3d::ApplyAffine(placement, model.vertices);
			model.crs = crs;
			model.ComputeBBox();
			payload = SerializeGeomPayload(model);
		}
		writer.WriteValue(string_t(reinterpret_cast<const char *>(payload.data()), payload.size()));
	}
}

void RegisterTransformFunctions(ExtensionLoader &loader, const LogicalType &solid_3d_type,
                                const LogicalType &geom_3d_type) {
	// Transform functions
	ScalarFunctionSet translate_set("st_3dtranslate");
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
	rotatex_set.AddFunction(ScalarFunction({solid_3d_type, LogicalType::DOUBLE}, solid_3d_type, ST_3DRotateXSolidFun));
	rotatex_set.AddFunction(ScalarFunction({geom_3d_type, LogicalType::DOUBLE}, geom_3d_type, ST_3DRotateXGeomFun));
	RegisterDocumented(loader, std::move(rotatex_set),
	                   {{"geom", "radians"},
	                    "Rotates a SOLID_3D or GEOM_3D value about the X axis by an angle in radians, right-handed and "
	                    "counter-clockwise; the result has the input's type.",
	                    "ST_3DRotateX(ST_Geom3DFromWKB('POINT Z (1 2 3)'::GEOMETRY), pi() / 2)",
	                    {"transform"}});

	ScalarFunctionSet rotatey_set("st_3drotatey");
	rotatey_set.AddFunction(ScalarFunction({solid_3d_type, LogicalType::DOUBLE}, solid_3d_type, ST_3DRotateYSolidFun));
	rotatey_set.AddFunction(ScalarFunction({geom_3d_type, LogicalType::DOUBLE}, geom_3d_type, ST_3DRotateYGeomFun));
	RegisterDocumented(loader, std::move(rotatey_set),
	                   {{"geom", "radians"},
	                    "Rotates a SOLID_3D or GEOM_3D value about the Y axis by an angle in radians, right-handed and "
	                    "counter-clockwise; the result has the input's type.",
	                    "ST_3DRotateY(ST_Geom3DFromWKB('POINT Z (1 2 3)'::GEOMETRY), pi() / 2)",
	                    {"transform"}});

	ScalarFunctionSet rotatez_set("st_3drotatez");
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
	add_transform(ScalarFunction({solid_3d_type, LogicalType::INTEGER, LogicalType::INTEGER}, solid_3d_type,
	                             ST_3DTransformIntFun));
	add_transform(
	    ScalarFunction({geom_3d_type, LogicalType::INTEGER, LogicalType::INTEGER}, geom_3d_type, ST_3DTransformIntFun));
	add_transform(ScalarFunction({solid_3d_type, LogicalType::VARCHAR, LogicalType::VARCHAR}, solid_3d_type,
	                             ST_3DTransformStrFun));
	add_transform(
	    ScalarFunction({geom_3d_type, LogicalType::VARCHAR, LogicalType::VARCHAR}, geom_3d_type, ST_3DTransformStrFun));
	add_transform(ScalarFunction({solid_3d_type, LogicalType::INTEGER}, solid_3d_type, ST_3DTransformToIntFun));
	add_transform(ScalarFunction({geom_3d_type, LogicalType::INTEGER}, geom_3d_type, ST_3DTransformToIntFun));
	add_transform(ScalarFunction({solid_3d_type, LogicalType::VARCHAR}, solid_3d_type, ST_3DTransformToStrFun));
	add_transform(ScalarFunction({geom_3d_type, LogicalType::VARCHAR}, geom_3d_type, ST_3DTransformToStrFun));
	RegisterDocumented(
	    loader, std::move(transform_set),
	    vector<FunctionDocs> {
	        {{"geom", "target_crs"},
	         "Reprojects a SOLID_3D or GEOM_3D value from its own CRS (ST_3DCRS; raises when it has none) to a CRS "
	         "given as an EPSG integer or a CRS string; X and Y are reprojected, Z passes through unchanged, and the "
	         "result is in the target CRS. Solids are re-validated afterwards.",
	         "ST_3DTransform(ST_3DSetCRS(ST_Geom3DFromWKB('POINT Z (85000 446800 5)'::GEOMETRY), 'EPSG:28992'), 4326)",
	         {"transform"}},
	        {{"geom", "source_crs", "target_crs"},
	         "Reprojects a SOLID_3D or GEOM_3D value between CRSs given as EPSG integers or CRS strings; a value that "
	         "has its own CRS must be in the source. X and Y are reprojected, Z passes through unchanged, and the "
	         "result is in the target CRS. Solids are re-validated afterwards.",
	         "ST_3DTransform(ST_Geom3DFromWKB('POINT Z (85000 446800 5)'::GEOMETRY), 28992, 4326)",
	         {"transform"}}});
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
