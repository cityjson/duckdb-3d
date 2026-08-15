#include "functions/three_d_functions.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/function/function_set.hpp"
#include "duckdb/function/scalar_function.hpp"

#include "kernel/metadata_parser.hpp"
#include "kernel/model_builder.hpp"
#include "kernel/wkb_export.hpp"
#include "kernel/wkb_parser.hpp"

#include <string>
#include <vector>

namespace duckdb {

// ──────────────────────────────────────────────────────────────
// ST_3DFromWKB(wkb BLOB) → SOLID_3D (BLOB)
// ──────────────────────────────────────────────────────────────
static void ST_3DFromWKBFun(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &wkb_vec = args.data[0];
	UnaryExecutor::Execute<string_t, string_t>(wkb_vec, result, args.size(), [&](string_t wkb) {
		using namespace duckdb_3d;
		auto surfaces = ParseWKB(reinterpret_cast<const uint8_t *>(wkb.GetData()), wkb.GetSize());
		auto model = BuildSolidModel(surfaces);
		auto payload = SerializePayload(model);
		return StringVector::AddStringOrBlob(result,
		                                     string_t(reinterpret_cast<const char *>(payload.data()), payload.size()));
	});
}

// ──────────────────────────────────────────────────────────────
// ST_3DTryFromWKB(wkb BLOB) → SOLID_3D (BLOB) or NULL
// ──────────────────────────────────────────────────────────────
static void ST_3DTryFromWKBFun(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &wkb_vec = args.data[0];
	UnaryExecutor::ExecuteWithNulls<string_t, string_t>(
	    wkb_vec, result, args.size(), [&](string_t wkb, ValidityMask &mask, idx_t idx) -> string_t {
		    using namespace duckdb_3d;
		    try {
			    auto surfaces = ParseWKB(reinterpret_cast<const uint8_t *>(wkb.GetData()), wkb.GetSize());
			    auto model = BuildSolidModel(surfaces);
			    auto payload = SerializePayload(model);
			    return StringVector::AddStringOrBlob(
			        result, string_t(reinterpret_cast<const char *>(payload.data()), payload.size()));
		    } catch (...) {
			    mask.SetInvalid(idx);
			    return string_t();
		    }
	    });
}

// ──────────────────────────────────────────────────────────────
// ST_3DFromWKB(wkb BLOB, geometry_properties STRUCT) → SOLID_3D
//
// A CityParquet package stores geometry_properties_lod* as a native STRUCT
// (spec §8), so we accept it directly rather than forcing a to_json() round-trip.
// Only `type` and `shells` are consumed (the same fields the JSON path reads);
// `surfaces` / `face_semantics` and any producer extras are ignored. The struct
// overload is registered as (BLOB, ANY) with a bind that normalises the struct
// and routes plain VARCHAR / JSON / SQLNULL metadata back to the JSON executor,
// so it is a strict superset of the VARCHAR overload.
//
// The struct itself is read row-by-row by the shared, name-resolved
// ReadGeometryPropertiesStructRow (functions/struct_metadata.cpp), which the
// arrow-native STRUCT overloads use too — so the bind needs no per-field index
// bookkeeping, only the type normalisation and the missing-`shells` diagnosis.
// ──────────────────────────────────────────────────────────────

// ──────────────────────────────────────────────────────────────
// ST_3DFromWKB / ST_3DTryFromWKB(wkb BLOB, geometry_properties) → SOLID_3D
//
// One executor covers all four metadata overloads: {plain, TRY} × {JSON
// VARCHAR metadata, bind-normalised geometry_properties STRUCT}. The plain
// variants propagate kernel errors; the TRY variants yield NULL per offending
// row instead.
// ──────────────────────────────────────────────────────────────

//! Shared executor for the four st_3dfromwkb/st_3dtryfromwkb metadata
//! overloads. TRY_VARIANT wraps each row in a catch-all that yields NULL;
//! SOURCE selects JSON-text parsing vs the bind-normalised STRUCT reader.
template <bool TRY_VARIANT, MetaSource SOURCE>
static void FromWKBWithMetaExecutor(DataChunk &args, ExpressionState &state, Vector &result) {
	auto count = args.size();
	// Capture constness before Flatten mutates args.data[1]: a constant-folded
	// call must return a constant result (DuckDB asserts this in debug builds).
	bool all_constant = args.AllConstant();
	auto &wkb_vec = args.data[0];
	auto &meta_vec = args.data[1];

	UnifiedVectorFormat wkb_data;
	wkb_vec.ToUnifiedFormat(count, wkb_data);
	auto wkb_strings = UnifiedVectorFormat::GetData<string_t>(wkb_data);

	UnifiedVectorFormat meta_data;
	const string_t *meta_strings = nullptr;
	if constexpr (SOURCE == MetaSource::STRUCT_FIELDS) {
		meta_vec.Flatten(count);
	} else {
		meta_vec.ToUnifiedFormat(count, meta_data);
		meta_strings = UnifiedVectorFormat::GetData<string_t>(meta_data);
	}

	auto &result_validity = FlatVector::Validity(result);
	auto result_data = FlatVector::GetData<string_t>(result);

	for (idx_t i = 0; i < count; i++) {
		auto wkb_idx = wkb_data.sel->get_index(i);
		if (!wkb_data.validity.RowIsValid(wkb_idx)) {
			result_validity.SetInvalid(i);
			result_data[i] = string_t();
			continue;
		}
		auto process_row = [&]() {
			using namespace duckdb_3d;
			auto &wkb = wkb_strings[wkb_idx];
			auto surfaces = ParseWKB(reinterpret_cast<const uint8_t *>(wkb.GetData()), wkb.GetSize());

			bool meta_valid;
			if constexpr (SOURCE == MetaSource::STRUCT_FIELDS) {
				meta_valid = FlatVector::Validity(meta_vec).RowIsValid(i);
			} else {
				meta_valid = meta_data.validity.RowIsValid(meta_data.sel->get_index(i));
			}

			SolidModel model;
			if (meta_valid) {
				GeometryMetadata metadata;
				if constexpr (SOURCE == MetaSource::STRUCT_FIELDS) {
					metadata = ReadGeometryPropertiesStructRow(meta_vec, count, i);
				} else {
					auto &meta_str = meta_strings[meta_data.sel->get_index(i)];
					metadata = ParseGeometryProperties(std::string(meta_str.GetData(), meta_str.GetSize()));
				}
				model = BuildSolidModel(surfaces, metadata);
			} else {
				model = BuildSolidModel(surfaces);
			}
			auto payload = SerializePayload(model);
			result_data[i] = StringVector::AddStringOrBlob(
			    result, string_t(reinterpret_cast<const char *>(payload.data()), payload.size()));
		};
		if constexpr (TRY_VARIANT) {
			try {
				process_row();
			} catch (...) {
				result_validity.SetInvalid(i);
				result_data[i] = string_t();
			}
		} else {
			process_row();
		}
	}

	if (all_constant) {
		result.SetVectorType(VectorType::CONSTANT_VECTOR);
	}
}

// Bind for the (BLOB, ANY) metadata overload. Routes SQLNULL / VARCHAR / JSON
// metadata to the JSON executor (`varchar_fn`) and a STRUCT to `struct_fn` after
// normalising the struct's `type` → VARCHAR and `shells` → LIST(LIST(INTEGER)).
static unique_ptr<FunctionData> BindWkbMetaAny(ScalarFunction &bound_function,
                                               vector<unique_ptr<Expression>> &arguments, scalar_function_t varchar_fn,
                                               scalar_function_t struct_fn) {
	auto &meta_type = arguments[1]->return_type;
	switch (meta_type.id()) {
	case LogicalTypeId::UNKNOWN:
		// A prepared-statement '?' parameter: defer to a later re-bind.
		throw ParameterNotResolvedException();
	case LogicalTypeId::SQLNULL:
	case LogicalTypeId::VARCHAR:
		// Plain / JSON-alias / NULL metadata: reuse the JSON executor unchanged.
		bound_function.arguments[1] = LogicalType::VARCHAR;
		bound_function.function = varchar_fn;
		return nullptr;
	case LogicalTypeId::STRUCT: {
		auto &child_types = StructType::GetChildTypes(meta_type);
		bool has_shells = false;
		child_list_t<LogicalType> normalized;
		normalized.reserve(child_types.size());
		for (idx_t i = 0; i < child_types.size(); i++) {
			auto &name = child_types[i].first;
			auto normalized_type = child_types[i].second;
			if (StringUtil::CIEquals(name, "shells")) {
				has_shells = true;
				// HUGEINT so every standard integer producer type (through UBIGINT)
				// widens without a bind-time cast failure; the executor range-checks
				// each value, so ST_3DTryFromWKB can turn an out-of-range count into
				// NULL instead of raising during the (pre-executor) struct cast.
				normalized_type = LogicalType::LIST(LogicalType::LIST(LogicalType::HUGEINT));
			} else if (StringUtil::CIEquals(name, "type")) {
				normalized_type = LogicalType::VARCHAR;
			}
			normalized.emplace_back(name, std::move(normalized_type));
		}
		if (!has_shells) {
			throw BinderException("ST_3DFromWKB: geometry_properties metadata STRUCT must contain a `shells` "
			                      "field; pass the metadata as a JSON VARCHAR otherwise");
		}
		bound_function.arguments[1] = LogicalType::STRUCT(std::move(normalized));
		bound_function.function = struct_fn;
		// The executor resolves `type`/`shells` by name from the normalised type,
		// so no bind data is needed.
		return nullptr;
	}
	default:
		throw BinderException("ST_3DFromWKB: metadata must be a geometry_properties STRUCT or a JSON VARCHAR, got " +
		                      meta_type.ToString());
	}
}

static unique_ptr<FunctionData> FromWkbAnyBind(ClientContext &, ScalarFunction &bound_function,
                                               vector<unique_ptr<Expression>> &arguments) {
	return BindWkbMetaAny(bound_function, arguments, FromWKBWithMetaExecutor<false, MetaSource::JSON_TEXT>,
	                      FromWKBWithMetaExecutor<false, MetaSource::STRUCT_FIELDS>);
}

static unique_ptr<FunctionData> TryFromWkbAnyBind(ClientContext &, ScalarFunction &bound_function,
                                                  vector<unique_ptr<Expression>> &arguments) {
	return BindWkbMetaAny(bound_function, arguments, FromWKBWithMetaExecutor<true, MetaSource::JSON_TEXT>,
	                      FromWKBWithMetaExecutor<true, MetaSource::STRUCT_FIELDS>);
}

// ──────────────────────────────────────────────────────────────
// ST_3DAsWKB(solid SOLID_3D) → BLOB
// ──────────────────────────────────────────────────────────────
static void ST_3DAsWKBFun(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &solid_vec = args.data[0];
	UnaryExecutor::Execute<string_t, string_t>(solid_vec, result, args.size(), [&](string_t solid) {
		using namespace duckdb_3d;
		auto model = DeserializePayload(reinterpret_cast<const uint8_t *>(solid.GetData()), solid.GetSize());
		auto wkb = ExportWKB(model);
		return StringVector::AddStringOrBlob(result, string_t(reinterpret_cast<const char *>(wkb.data()), wkb.size()));
	});
}

void RegisterSolidIOFunctions(ExtensionLoader &loader, const LogicalType &solid_3d_type) {
	// ST_3DFromWKB: 1-arg, 2-arg (VARCHAR), and 2-arg (STRUCT) overloads.
	// The constructors return the SOLID_3D alias so their result carries the type
	// through to the typed consumer overloads without an explicit cast.
	ScalarFunctionSet from_wkb_set("st_3dfromwkb");
	from_wkb_set.AddFunction(ScalarFunction({LogicalType::BLOB}, solid_3d_type, ST_3DFromWKBFun));
	auto from_wkb_2arg = ScalarFunction({LogicalType::BLOB, LogicalType::VARCHAR}, solid_3d_type,
	                                    FromWKBWithMetaExecutor<false, MetaSource::JSON_TEXT>);
	from_wkb_2arg.SetNullHandling(FunctionNullHandling::SPECIAL_HANDLING);
	from_wkb_set.AddFunction(from_wkb_2arg);
	// (BLOB, ANY): accept CityParquet's geometry_properties STRUCT directly; the
	// bind routes VARCHAR/JSON/SQLNULL back to the JSON executor.
	auto from_wkb_any = ScalarFunction({LogicalType::BLOB, LogicalType::ANY}, solid_3d_type,
	                                   FromWKBWithMetaExecutor<false, MetaSource::STRUCT_FIELDS>, FromWkbAnyBind);
	from_wkb_any.SetNullHandling(FunctionNullHandling::SPECIAL_HANDLING);
	from_wkb_set.AddFunction(from_wkb_any);
	loader.RegisterFunction(from_wkb_set);

	// ST_3DTryFromWKB: 1-arg, 2-arg (VARCHAR), and 2-arg (STRUCT) overloads
	ScalarFunctionSet try_from_wkb_set("st_3dtryfromwkb");
	try_from_wkb_set.AddFunction(ScalarFunction({LogicalType::BLOB}, solid_3d_type, ST_3DTryFromWKBFun));
	auto try_from_wkb_2arg = ScalarFunction({LogicalType::BLOB, LogicalType::VARCHAR}, solid_3d_type,
	                                        FromWKBWithMetaExecutor<true, MetaSource::JSON_TEXT>);
	try_from_wkb_2arg.SetNullHandling(FunctionNullHandling::SPECIAL_HANDLING);
	try_from_wkb_set.AddFunction(try_from_wkb_2arg);
	auto try_from_wkb_any = ScalarFunction({LogicalType::BLOB, LogicalType::ANY}, solid_3d_type,
	                                       FromWKBWithMetaExecutor<true, MetaSource::STRUCT_FIELDS>, TryFromWkbAnyBind);
	try_from_wkb_any.SetNullHandling(FunctionNullHandling::SPECIAL_HANDLING);
	try_from_wkb_set.AddFunction(try_from_wkb_any);
	loader.RegisterFunction(try_from_wkb_set);

	// ST_3DAsWKB(solid SOLID_3D) -> BLOB. The BLOB overload keeps stored/legacy
	// payloads working; DuckDB resolves the alias exactly, so both are needed.
	ScalarFunctionSet as_wkb_set("st_3daswkb");
	as_wkb_set.AddFunction(ScalarFunction({LogicalType::BLOB}, LogicalType::BLOB, ST_3DAsWKBFun));
	as_wkb_set.AddFunction(ScalarFunction({solid_3d_type}, LogicalType::BLOB, ST_3DAsWKBFun));
	loader.RegisterFunction(as_wkb_set);
}

} // namespace duckdb
