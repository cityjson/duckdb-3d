#include "functions/three_d_functions.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/types/geometry.hpp"
#include "duckdb/common/vector/vector_iterator.hpp"
#include "duckdb/common/vector/vector_writer.hpp"
#include "duckdb/function/function_set.hpp"
#include "duckdb/function/scalar_function.hpp"

#include "kernel/metadata_parser.hpp"
#include "kernel/model_builder.hpp"
#include "kernel/wkb_export.hpp"
#include "kernel/wkb_parser.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace duckdb {

// Kernel names this file uses unqualified. Using-declarations rather than a
// using-directive, which clang-tidy's google-build-using-namespace rejects.
using duckdb_3d::DeserializePayload;
using duckdb_3d::GeometryMetadata;
using duckdb_3d::ParseGeometryProperties;
using duckdb_3d::ParseWKB;
using duckdb_3d::SolidModel;

// ──────────────────────────────────────────────────────────────
// ST_3DFromWKB(wkb BLOB | GEOMETRY) → SOLID_3D (BLOB)
// ──────────────────────────────────────────────────────────────
//! `crs` is the CRS a GEOMETRY argument's type carries; WKB from a BLOB brings
//! its own, from an EWKB SRID.
static void FromWKBVector(const Vector &wkb_vec, Vector &result, const std::string &crs) {
	UnaryExecutor::Execute<string_t, string_t>(wkb_vec, result, [&](string_t wkb) {
		auto surfaces = ParseWKB(reinterpret_cast<const uint8_t *>(wkb.GetData()), wkb.GetSize());
		auto model = BuildSolidModel(surfaces);
		if (!crs.empty()) {
			model.crs = crs;
		}
		auto payload = SerializePayload(model);
		return StringVector::AddStringOrBlob(result,
		                                     string_t(reinterpret_cast<const char *>(payload.data()), payload.size()));
	});
}

static void ST_3DFromWKBFun(DataChunk &args, ExpressionState &state, Vector &result) {
	FromWKBVector(args.data[0], result, std::string());
}

// ──────────────────────────────────────────────────────────────
// ST_3DTryFromWKB(wkb BLOB | GEOMETRY) → SOLID_3D (BLOB) or NULL
// ──────────────────────────────────────────────────────────────
static void TryFromWKBVector(const Vector &wkb_vec, Vector &result, const std::string &crs) {
	UnaryExecutor::Execute<string_t, string_t>(wkb_vec, result, [&](string_t wkb) -> optional<string_t> {
		try {
			auto surfaces = ParseWKB(reinterpret_cast<const uint8_t *>(wkb.GetData()), wkb.GetSize());
			auto model = BuildSolidModel(surfaces);
			if (!crs.empty()) {
				model.crs = crs;
			}
			auto payload = SerializePayload(model);
			return StringVector::AddStringOrBlob(
			    result, string_t(reinterpret_cast<const char *>(payload.data()), payload.size()));
		} catch (const Exception &) {
			throw;
		} catch (const std::bad_alloc &) {
			throw;
		} catch (const std::exception &) {
			// A kernel error: this row's input is malformed.
			return {};
		}
	});
}

static void ST_3DTryFromWKBFun(DataChunk &args, ExpressionState &state, Vector &result) {
	TryFromWKBVector(args.data[0], result, std::string());
}

// ──────────────────────────────────────────────────────────────
// ST_3DFromWKB / ST_3DTryFromWKB(geom GEOMETRY) → SOLID_3D
//
// A GeoParquet-legal column reaches a query as the Parquet GEOMETRY logical type
// rather than BLOB. Geometry::ToBinary is the supported route down to WKB: the
// storage is physically a string, but not contractually raw WKB, so the string_t
// must not simply be reinterpreted.
// ──────────────────────────────────────────────────────────────
static void ST_3DFromGeometryFun(DataChunk &args, ExpressionState &state, Vector &result) {
	Vector wkb_vec(LogicalType::BLOB);
	Geometry::ToBinary(args.data[0], wkb_vec);
	FromWKBVector(wkb_vec, result, BoundCrs(state));
}

static void ST_3DTryFromGeometryFun(DataChunk &args, ExpressionState &state, Vector &result) {
	Vector wkb_vec(LogicalType::BLOB);
	Geometry::ToBinary(args.data[0], wkb_vec);
	TryFromWKBVector(wkb_vec, result, BoundCrs(state));
}

static unique_ptr<FunctionData> BindFromWkbArg(BindScalarFunctionInput &input) {
	return BindWkbArgument(input, ST_3DFromWKBFun, ST_3DFromGeometryFun);
}

static unique_ptr<FunctionData> BindTryFromWkbArg(BindScalarFunctionInput &input) {
	return BindWkbArgument(input, ST_3DTryFromWKBFun, ST_3DTryFromGeometryFun);
}

// ──────────────────────────────────────────────────────────────
// ST_3DFromWKB / ST_3DTryFromWKB(wkb, geometry_properties) → SOLID_3D
//
// One (ANY, ANY) candidate covers every metadata overload, resolved at bind:
//   * wkb — BLOB, or the core GEOMETRY a Parquet-annotated column carries
//     (converted to WKB with Geometry::ToBinary, as the one-argument form does);
//   * geometry_properties — JSON text (VARCHAR / JSON / NULL) or the CityParquet
//     geometry_properties_lod* STRUCT, read directly with no to_json() round trip.
// A single candidate keeps an untyped NULL in either slot unambiguous: separate
// BLOB and GEOMETRY overloads would tie on its cast cost.
//
// Only `type` and `shells` are consumed; `surfaces` (JSON or VARCHAR),
// `face_semantics` and any producer extras are ignored. The struct is read
// row-by-row by the shared, name-resolved ReadGeometryPropertiesStructRow
// (functions/struct_metadata.cpp), so the bind needs no per-field index
// bookkeeping, only the type normalisation and the missing-`shells` diagnosis.
// The plain variants propagate kernel errors; the TRY variants yield NULL per
// offending row instead.
// ──────────────────────────────────────────────────────────────

//! Reads one row's geometry_properties argument, from JSON text or from the
//! bind-normalised STRUCT.
template <MetaSource SOURCE>
class MetaReader;

template <>
class MetaReader<MetaSource::JSON_TEXT> {
public:
	MetaReader(Vector &meta_vec, idx_t) : values(meta_vec.Values<string_t>()) {
	}
	bool IsValid(idx_t row) const {
		return values[row].IsValid();
	}
	GeometryMetadata Read(idx_t row) const {
		auto &text = values[row].GetValue();
		return ParseGeometryProperties(std::string_view(text.GetData(), text.GetSize()));
	}

private:
	VectorIterator<string_t> values;
};

template <>
class MetaReader<MetaSource::STRUCT_FIELDS> {
public:
	MetaReader(Vector &meta_vec_p, idx_t count_p) : meta_vec(meta_vec_p), count(count_p) {
		meta_vec.Flatten();
	}
	bool IsValid(idx_t row) const {
		return FlatVector::Validity(meta_vec).RowIsValid(row);
	}
	GeometryMetadata Read(idx_t row) const {
		return ReadGeometryPropertiesStructRow(meta_vec, count, row);
	}

private:
	Vector &meta_vec;
	idx_t count;
};

//! Shared executor for the st_3dfromwkb/st_3dtryfromwkb metadata overloads.
//! TRY_VARIANT wraps each row in a catch-all that yields NULL; SOURCE selects
//! JSON-text parsing vs the bind-normalised STRUCT reader; GEOMETRY_INPUT
//! converts a core GEOMETRY argument to WKB first.
template <bool TRY_VARIANT, MetaSource SOURCE, bool GEOMETRY_INPUT>
static void FromWKBWithMetaExecutor(DataChunk &args, ExpressionState &state, Vector &result) {
	auto count = args.size();
	MetaReader<SOURCE> meta(args.data[1], count);

	Vector converted_wkb(LogicalType::BLOB);
	if constexpr (GEOMETRY_INPUT) {
		Geometry::ToBinary(args.data[0], converted_wkb);
	}
	auto &wkb_vec = GEOMETRY_INPUT ? converted_wkb : args.data[0];
	const auto crs = BoundCrs(state);
	auto wkb_values = wkb_vec.Values<string_t>();

	auto writer = FlatVector::Writer<string_t>(result, count);
	for (idx_t i = 0; i < count; i++) {
		auto wkb = wkb_values[i];
		if (!wkb.IsValid()) {
			writer.WriteNull();
			continue;
		}
		auto build_payload = [&]() {
			auto &wkb_value = wkb.GetValue();
			auto surfaces = ParseWKB(reinterpret_cast<const uint8_t *>(wkb_value.GetData()), wkb_value.GetSize());
			auto model = meta.IsValid(i) ? BuildSolidModel(surfaces, meta.Read(i)) : BuildSolidModel(surfaces);
			if (!crs.empty()) {
				model.crs = crs;
			}
			return SerializePayload(model);
		};
		if constexpr (TRY_VARIANT) {
			try {
				auto payload = build_payload();
				writer.WriteValue(string_t(reinterpret_cast<const char *>(payload.data()), payload.size()));
			} catch (const InvalidInputException &) {
				// A malformed geometry_properties STRUCT row.
				writer.WriteNull();
			} catch (const Exception &) {
				throw;
			} catch (const std::bad_alloc &) {
				throw;
			} catch (const std::exception &) {
				// A kernel error: this row's input is malformed.
				writer.WriteNull();
			}
		} else {
			auto payload = build_payload();
			writer.WriteValue(string_t(reinterpret_cast<const char *>(payload.data()), payload.size()));
		}
	}
}

//! Argument resolution for the (ANY, ANY) metadata candidate. Resolves the WKB
//! slot to BLOB or GEOMETRY and the metadata slot to JSON text (VARCHAR / JSON /
//! NULL) or a STRUCT normalised to `type` → VARCHAR and `shells` →
//! LIST(LIST(HUGEINT)).
static void ResolveWkbMeta(ResolveScalarFunctionTypesInput &input) {
	auto &wkb_type = input.GetArgumentType(0);
	auto &meta_type = input.GetArgumentType(1);
	if (wkb_type.id() == LogicalTypeId::UNKNOWN || meta_type.id() == LogicalTypeId::UNKNOWN) {
		// A prepared-statement '?' parameter: defer to a later re-bind.
		throw ParameterNotResolvedException();
	}
	auto &arguments = input.GetBoundFunction().GetArguments();
	// Keep a GEOMETRY argument's own type (CRS parameter and all) so no cast is
	// added; anything else is cast to BLOB, which rejects non-WKB types at bind.
	arguments[0] = wkb_type.id() == LogicalTypeId::GEOMETRY ? wkb_type : LogicalType::BLOB;

	switch (meta_type.id()) {
	case LogicalTypeId::SQLNULL:
	case LogicalTypeId::STRING_LITERAL:
	case LogicalTypeId::VARCHAR:
		// Plain / JSON-alias / NULL metadata: JSON text.
		arguments[1] = LogicalType::VARCHAR;
		return;
	case LogicalTypeId::STRUCT: {
		auto &child_types = StructType::GetChildTypes(meta_type);
		bool has_shells = false;
		child_list_t<LogicalType> normalized;
		normalized.reserve(child_types.size());
		for (idx_t i = 0; i < child_types.size(); i++) {
			auto &name = child_types[i].first;
			auto normalized_type = child_types[i].second;
			if (name == "shells") {
				has_shells = true;
				// HUGEINT so every standard integer producer type (through UBIGINT)
				// widens without a bind-time cast failure; the executor range-checks
				// each value, so ST_3DTryFromWKB can turn an out-of-range count into
				// NULL instead of raising during the (pre-executor) struct cast.
				normalized_type = LogicalType::LIST(LogicalType::LIST(LogicalType::HUGEINT));
			} else if (name == "type") {
				normalized_type = LogicalType::VARCHAR;
			}
			normalized.emplace_back(name, std::move(normalized_type));
		}
		if (!has_shells) {
			throw BinderException("ST_3DFromWKB: geometry_properties metadata STRUCT must contain a `shells` "
			                      "field; pass the metadata as a JSON VARCHAR otherwise");
		}
		arguments[1] = LogicalType::STRUCT(std::move(normalized));
		return;
	}
	default:
		throw BinderException("ST_3DFromWKB: metadata must be a geometry_properties STRUCT or a JSON VARCHAR, got " +
		                      meta_type.ToString());
	}
}

//! Bind for the (ANY, ANY) metadata candidate: picks the executor instantiation
//! for the argument types ResolveWkbMeta settled on. The executor resolves
//! `type`/`shells` by name from the normalised type; the bind data carries only a
//! GEOMETRY argument's CRS.
template <bool TRY_VARIANT>
static unique_ptr<FunctionData> BindWkbMeta(BindScalarFunctionInput &input) {
	auto &bound_function = input.GetBoundFunction();
	auto &arguments = bound_function.GetArguments();
	const bool geometry_input = arguments[0].id() == LogicalTypeId::GEOMETRY;
	scalar_function_t executor;
	if (arguments[1].id() == LogicalTypeId::STRUCT) {
		executor = geometry_input ? FromWKBWithMetaExecutor<TRY_VARIANT, MetaSource::STRUCT_FIELDS, true>
		                          : FromWKBWithMetaExecutor<TRY_VARIANT, MetaSource::STRUCT_FIELDS, false>;
	} else {
		executor = geometry_input ? FromWKBWithMetaExecutor<TRY_VARIANT, MetaSource::JSON_TEXT, true>
		                          : FromWKBWithMetaExecutor<TRY_VARIANT, MetaSource::JSON_TEXT, false>;
	}
	bound_function.SetFunctionCallback(KernelErrorsAsInvalidInput(executor));
	return GeometryArgumentCrs(arguments[0]);
}

// ──────────────────────────────────────────────────────────────
// ST_3DAsWKB(solid SOLID_3D) → BLOB
// ──────────────────────────────────────────────────────────────
static void ST_3DAsWKBFun(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &solid_vec = args.data[0];
	UnaryExecutor::Execute<string_t, string_t>(solid_vec, result, [&](string_t solid) {
		auto model = DeserializePayload(reinterpret_cast<const uint8_t *>(solid.GetData()), solid.GetSize());
		auto wkb = ExportWKB(model);
		return StringVector::AddStringOrBlob(result, string_t(reinterpret_cast<const char *>(wkb.data()), wkb.size()));
	});
}

void RegisterSolidIOFunctions(ExtensionLoader &loader, const LogicalType &solid_3d_type) {
	// ST_3DFromWKB: 1-arg and 2-arg candidates, each resolved at bind time.
	// The constructors return the SOLID_3D alias so their result carries the type
	// through to the typed consumer overloads without an explicit cast.
	ScalarFunctionSet from_wkb_set("st_3dfromwkb");
	ScalarFunction from_wkb({LogicalType::ANY}, solid_3d_type, ST_3DFromWKBFun, BindFromWkbArg);
	from_wkb.SetResolveTypesCallback(ResolveWkbArgument);
	from_wkb_set.AddFunction(from_wkb);
	// (ANY, ANY): BLOB or GEOMETRY WKB, with JSON-text or STRUCT metadata; the
	// bind picks the executor. The placeholder function is replaced at bind.
	auto from_wkb_meta =
	    ScalarFunction({LogicalType::ANY, LogicalType::ANY}, solid_3d_type,
	                   FromWKBWithMetaExecutor<false, MetaSource::JSON_TEXT, false>, BindWkbMeta<false>);
	from_wkb_meta.SetResolveTypesCallback(ResolveWkbMeta);
	from_wkb_meta.SetNullHandling(FunctionNullHandling::SPECIAL_HANDLING);
	from_wkb_set.AddFunction(from_wkb_meta);
	RegisterDocumented(
	    loader, std::move(from_wkb_set),
	    {{"wkb", "geometry_properties"},
	     "Builds a SOLID_3D from PolyhedralSurface Z WKB, or a GeometryCollection Z of them for a "
	     "multi-solid, as BLOB or GEOMETRY; the optional geometry_properties (JSON text or a CityParquet STRUCT) "
	     "restores the shell grouping WKB cannot carry. Raises on unparseable WKB or unsupported topology.",
	     "ST_3DFromWKB(ST_3DAsWKB(ST_3DExtrude(ST_Geom3DFromWKB('POLYGON Z ((0 0 0, 2 0 0, 2 2 0, 0 2 0, 0 0 "
	     "0))'::GEOMETRY), 3.0)))",
	     {"import"}});

	// ST_3DTryFromWKB: the same two shapes
	ScalarFunctionSet try_from_wkb_set("st_3dtryfromwkb");
	ScalarFunction try_from_wkb({LogicalType::ANY}, solid_3d_type, ST_3DTryFromWKBFun, BindTryFromWkbArg);
	try_from_wkb.SetResolveTypesCallback(ResolveWkbArgument);
	try_from_wkb_set.AddFunction(try_from_wkb);
	auto try_from_wkb_meta =
	    ScalarFunction({LogicalType::ANY, LogicalType::ANY}, solid_3d_type,
	                   FromWKBWithMetaExecutor<true, MetaSource::JSON_TEXT, false>, BindWkbMeta<true>);
	try_from_wkb_meta.SetResolveTypesCallback(ResolveWkbMeta);
	try_from_wkb_meta.SetNullHandling(FunctionNullHandling::SPECIAL_HANDLING);
	try_from_wkb_set.AddFunction(try_from_wkb_meta);
	RegisterDocumented(loader, std::move(try_from_wkb_set),
	                   {{"wkb", "geometry_properties"},
	                    "Like ST_3DFromWKB, but returns NULL for a row whose WKB is unparseable or not a solid instead "
	                    "of raising; bind-time errors still raise.",
	                    "ST_3DTryFromWKB('POINT Z (1 2 3)'::GEOMETRY)",
	                    {"import"}});

	// ST_3DAsWKB(solid SOLID_3D) -> BLOB.
	ScalarFunctionSet as_wkb_set("st_3daswkb");
	as_wkb_set.AddFunction(ScalarFunction({solid_3d_type}, LogicalType::BLOB, ST_3DAsWKBFun));
	RegisterDocumented(
	    loader, std::move(as_wkb_set),
	    {{"solid"},
	     "Exports a SOLID_3D as OGC WKB: a PolyhedralSurface Z for one solid, a GeometryCollection Z for "
	     "a multi-solid.",
	     "ST_3DAsWKB(ST_3DExtrude(ST_Geom3DFromWKB('POLYGON Z ((0 0 0, 2 0 0, 2 2 0, 0 2 0, 0 0 0))'::GEOMETRY), 3.0))",
	     {"export"}});
}

} // namespace duckdb
