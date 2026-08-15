#include "functions/three_d_functions.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/types/hugeint.hpp"
#include "duckdb/function/function_set.hpp"
#include "duckdb/function/scalar_function.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"

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
// ──────────────────────────────────────────────────────────────
struct FromWkbStructBindData : public FunctionData {
	idx_t shells_index;
	bool has_type;
	idx_t type_index;

	FromWkbStructBindData(idx_t shells_index_p, bool has_type_p, idx_t type_index_p)
	    : shells_index(shells_index_p), has_type(has_type_p), type_index(type_index_p) {
	}

	unique_ptr<FunctionData> Copy() const override {
		return make_uniq<FromWkbStructBindData>(shells_index, has_type, type_index);
	}
	bool Equals(const FunctionData &other_p) const override {
		auto &o = other_p.Cast<FromWkbStructBindData>();
		return shells_index == o.shells_index && has_type == o.has_type && type_index == o.type_index;
	}
};

// Extract GeometryMetadata (type + shells) from row `row` of the flattened
// struct children. `shells_vec` is a normalised LIST(LIST(INTEGER)); `type_vec`
// (when present) is a normalised VARCHAR.
static duckdb_3d::GeometryMetadata ReadStructRowMetadata(Vector *type_vec, Vector *shells_vec, idx_t row,
                                                         bool has_type) {
	duckdb_3d::GeometryMetadata md;
	if (has_type && type_vec) {
		auto &type_validity = FlatVector::Validity(*type_vec);
		if (type_validity.RowIsValid(row)) {
			auto s = FlatVector::GetData<string_t>(*type_vec)[row];
			md.type = std::string(s.GetData(), s.GetSize());
		}
	}
	auto &shells_validity = FlatVector::Validity(*shells_vec);
	if (shells_validity.RowIsValid(row)) {
		auto outer = ListVector::GetData(*shells_vec)[row];
		auto &inner_list = ListVector::GetEntry(*shells_vec); // LIST(HUGEINT)
		auto inner_data = ListVector::GetData(inner_list);
		auto &int_vec = ListVector::GetEntry(inner_list); // HUGEINT
		auto int_data = FlatVector::GetData<hugeint_t>(int_vec);
		auto &int_validity = FlatVector::Validity(int_vec);
		auto &inner_list_validity = FlatVector::Validity(inner_list);
		std::vector<std::vector<uint32_t>> shells;
		shells.reserve(outer.length);
		for (idx_t j = 0; j < outer.length; j++) {
			// A present `shells` value carries no null nested elements (spec §8):
			// a null per-solid array is malformed input, not "no shells". Reject it
			// rather than reading whatever sits behind the null slot, whose contents
			// are unspecified rather than merely wrong. The face-count level below
			// is checked the same way.
			if (!inner_list_validity.RowIsValid(outer.offset + j)) {
				throw InvalidInputException(
				    "geometry_properties: null shells entry (no nested list element may be null)");
			}
			auto ie = inner_data[outer.offset + j];
			std::vector<uint32_t> shell;
			shell.reserve(ie.length);
			for (idx_t k = 0; k < ie.length; k++) {
				idx_t pos = ie.offset + k;
				if (!int_validity.RowIsValid(pos)) {
					throw InvalidInputException(
					    "geometry_properties: null shell face count (no nested list element may be null)");
				}
				// shells is normalised to HUGEINT[][] by the bind (so any standard
				// integer producer type is accepted without a pre-executor cast
				// failure); the fits-in-uint32 check runs here — inside the TRY
				// variant's catch — so an out-of-range count fails cleanly. TryCast
				// also rejects negatives.
				uint32_t face_count;
				if (!Hugeint::TryCast<uint32_t>(int_data[pos], face_count)) {
					throw InvalidInputException("geometry_properties: shells face count out of range");
				}
				shell.push_back(face_count);
			}
			shells.push_back(std::move(shell));
		}
		md.shells = std::move(shells);
	}
	return md;
}

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
	Vector *shells_vec = nullptr;
	Vector *type_vec = nullptr;
	bool has_type = false;
	if constexpr (SOURCE == MetaSource::STRUCT_FIELDS) {
		auto &info = state.expr.Cast<BoundFunctionExpression>().bind_info->Cast<FromWkbStructBindData>();
		meta_vec.Flatten(count);
		auto &children = StructVector::GetEntries(meta_vec);
		shells_vec = children[info.shells_index].get();
		shells_vec->Flatten(count);
		has_type = info.has_type;
		if (info.has_type) {
			type_vec = children[info.type_index].get();
			type_vec->Flatten(count);
		}
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
					metadata = ReadStructRowMetadata(type_vec, shells_vec, i, has_type);
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
		idx_t shells_index = DConstants::INVALID_INDEX;
		idx_t type_index = DConstants::INVALID_INDEX;
		bool has_type = false;
		child_list_t<LogicalType> normalized;
		normalized.reserve(child_types.size());
		for (idx_t i = 0; i < child_types.size(); i++) {
			auto &name = child_types[i].first;
			auto normalized_type = child_types[i].second;
			if (StringUtil::CIEquals(name, "shells")) {
				shells_index = i;
				// HUGEINT so every standard integer producer type (through UBIGINT)
				// widens without a bind-time cast failure; the executor range-checks
				// each value, so ST_3DTryFromWKB can turn an out-of-range count into
				// NULL instead of raising during the (pre-executor) struct cast.
				normalized_type = LogicalType::LIST(LogicalType::LIST(LogicalType::HUGEINT));
			} else if (StringUtil::CIEquals(name, "type")) {
				type_index = i;
				has_type = true;
				normalized_type = LogicalType::VARCHAR;
			}
			normalized.emplace_back(name, std::move(normalized_type));
		}
		if (shells_index == DConstants::INVALID_INDEX) {
			throw BinderException("ST_3DFromWKB: geometry_properties metadata STRUCT must contain a `shells` "
			                      "field; pass the metadata as a JSON VARCHAR otherwise");
		}
		bound_function.arguments[1] = LogicalType::STRUCT(std::move(normalized));
		bound_function.function = struct_fn;
		return make_uniq<FromWkbStructBindData>(shells_index, has_type, type_index);
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
	// ST_3DFromWKB: 1-arg, 2-arg (VARCHAR), and 2-arg (STRUCT) overloads
	ScalarFunctionSet from_wkb_set("st_3dfromwkb");
	from_wkb_set.AddFunction(ScalarFunction({LogicalType::BLOB}, LogicalType::BLOB, ST_3DFromWKBFun));
	auto from_wkb_2arg = ScalarFunction({LogicalType::BLOB, LogicalType::VARCHAR}, LogicalType::BLOB,
	                                    FromWKBWithMetaExecutor<false, MetaSource::JSON_TEXT>);
	from_wkb_2arg.SetNullHandling(FunctionNullHandling::SPECIAL_HANDLING);
	from_wkb_set.AddFunction(from_wkb_2arg);
	// (BLOB, ANY): accept CityParquet's geometry_properties STRUCT directly; the
	// bind routes VARCHAR/JSON/SQLNULL back to the JSON executor.
	auto from_wkb_any = ScalarFunction({LogicalType::BLOB, LogicalType::ANY}, LogicalType::BLOB,
	                                   FromWKBWithMetaExecutor<false, MetaSource::STRUCT_FIELDS>, FromWkbAnyBind);
	from_wkb_any.SetNullHandling(FunctionNullHandling::SPECIAL_HANDLING);
	from_wkb_set.AddFunction(from_wkb_any);
	loader.RegisterFunction(from_wkb_set);

	// ST_3DTryFromWKB: 1-arg, 2-arg (VARCHAR), and 2-arg (STRUCT) overloads
	ScalarFunctionSet try_from_wkb_set("st_3dtryfromwkb");
	try_from_wkb_set.AddFunction(ScalarFunction({LogicalType::BLOB}, LogicalType::BLOB, ST_3DTryFromWKBFun));
	auto try_from_wkb_2arg = ScalarFunction({LogicalType::BLOB, LogicalType::VARCHAR}, LogicalType::BLOB,
	                                        FromWKBWithMetaExecutor<true, MetaSource::JSON_TEXT>);
	try_from_wkb_2arg.SetNullHandling(FunctionNullHandling::SPECIAL_HANDLING);
	try_from_wkb_set.AddFunction(try_from_wkb_2arg);
	auto try_from_wkb_any = ScalarFunction({LogicalType::BLOB, LogicalType::ANY}, LogicalType::BLOB,
	                                       FromWKBWithMetaExecutor<true, MetaSource::STRUCT_FIELDS>, TryFromWkbAnyBind);
	try_from_wkb_any.SetNullHandling(FunctionNullHandling::SPECIAL_HANDLING);
	try_from_wkb_set.AddFunction(try_from_wkb_any);
	loader.RegisterFunction(try_from_wkb_set);

	// ST_3DAsWKB(solid SOLID_3D) -> BLOB
	loader.RegisterFunction(ScalarFunction("st_3daswkb", {LogicalType::BLOB}, LogicalType::BLOB, ST_3DAsWKBFun));
}

} // namespace duckdb
