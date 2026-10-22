#pragma once

#include "duckdb.hpp"
#include "duckdb/common/vector/list_vector.hpp"
#include "duckdb/common/vector/struct_vector.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/parser/parsed_data/create_scalar_function_info.hpp"
#include "kernel/geom_payload.hpp"
#include "kernel/metadata_parser.hpp"
#include "kernel/payload.hpp"

#include <cstring>
#include <new>
#include <utility>

namespace duckdb {

//! The kernel reports a geometry it cannot handle (a polygon with no tiling, a
//! solid volume refuses) with standard exceptions, which DuckDB treats as
//! internal and TRY() rethrows. Every registered function reports them as
//! InvalidInputException instead, a row-level error TRY() turns into NULL;
//! a bind that picks the callback itself wraps what it picks. Out of memory
//! passes through untouched, so DuckDB reports it as such and TRY() does not
//! hide it as NULL.
inline scalar_function_t KernelErrorsAsInvalidInput(const scalar_function_t &function) {
	return [function](DataChunk &args, ExpressionState &state, Vector &result) {
		try {
			function(args, state, result);
		} catch (Exception &) {
			throw;
		} catch (std::bad_alloc &) {
			throw;
		} catch (std::exception &e) {
			throw InvalidInputException(e.what());
		}
	};
}

//! Where a geometry_properties argument comes from (JSON VARCHAR vs STRUCT).
enum class MetaSource { JSON_TEXT, STRUCT_FIELDS };

//! Peek at the payload magic to decide whether a BLOB is a SOLID_3D or GEOM_3D value.
enum class PayloadKind { Solid, Geom, Unknown };

inline PayloadKind GetPayloadKind(const uint8_t *data, size_t size) {
	if (size >= 4) {
		if (std::memcmp(data, duckdb_3d::PAYLOAD_MAGIC, 4) == 0) {
			return PayloadKind::Solid;
		}
		if (std::memcmp(data, duckdb_3d::GEOM_PAYLOAD_MAGIC, 4) == 0) {
			return PayloadKind::Geom;
		}
	}
	return PayloadKind::Unknown;
}

//! A value's CRS, a string in its payload that is empty when there is none.
//! Defined in functions/crs.cpp.
std::string PayloadCrs(string_t payload);
//! Whether two CRS strings name one CRS: equal, or two spellings DuckDB's CRS
//! comparison identifies. Empty matches only empty.
bool SameCrs(const std::string &a, const std::string &b);
//! Raise for a function of two values in different CRSs (none counts as one).
void RequireSameCrs(const std::string &a, const std::string &b, const char *function);
//! The source CRS ST_3DTransform uses: `source` when given, which must agree with
//! the value's own `stored` CRS if it has one, else `stored`, which must exist.
std::string TransformSourceCrs(const std::string &stored, const std::string &source);

//! Wrap a function of two values (its first two arguments) so that a row whose
//! values are in different CRSs raises, named as `name`, before it computes.
inline scalar_function_t WithSameCrs(const scalar_function_t &function, const char *name) {
	return [function, name](DataChunk &args, ExpressionState &state, Vector &result) {
		auto a_values = args.data[0].Values<string_t>();
		auto b_values = args.data[1].Values<string_t>();
		for (idx_t i = 0; i < args.size(); i++) {
			auto a = a_values[i];
			auto b = b_values[i];
			if (a.IsValid() && b.IsValid()) {
				RequireSameCrs(PayloadCrs(a.GetValue()), PayloadCrs(b.GetValue()), name);
			}
		}
		function(args, state, result);
	};
}

//! The CRS a GEOMETRY argument's type carries, bound once per call site.
struct CrsBindData : public FunctionData {
	explicit CrsBindData(std::string crs) : crs(std::move(crs)) {
	}
	std::string crs;
	unique_ptr<FunctionData> Copy() const override;
	bool Equals(const FunctionData &other) const override;
};
//! Bind data for a WKB argument of `type`: its CRS for a GEOMETRY with one, else
//! nullptr.
unique_ptr<FunctionData> GeometryArgumentCrs(const LogicalType &type);
//! The CRS the call bound (see GeometryArgumentCrs), or empty.
std::string BoundCrs(ExpressionState &state);

//! Argument resolution for the single-argument WKB constructors. The argument
//! is declared ANY so that ONE candidate covers both the BLOB a plain column
//! carries and the GEOMETRY a Parquet-annotated column carries. Two separate
//! overloads would read more naturally but make an untyped `ST_3DFromWKB(NULL)`
//! ambiguous — SQLNULL casts to either at the same cost — and that call has
//! always bound. A GEOMETRY argument keeps its own type, CRS parameter and all,
//! so no cast is added; anything else is cast to BLOB.
inline void ResolveWkbArgument(ResolveScalarFunctionTypesInput &input) {
	auto &arg_type = input.GetArgumentType(0);
	if (arg_type.id() == LogicalTypeId::UNKNOWN) {
		// A prepared-statement '?' parameter: defer to a later re-bind.
		throw ParameterNotResolvedException();
	}
	input.GetBoundFunction().GetArguments()[0] =
	    arg_type.id() == LogicalTypeId::GEOMETRY ? arg_type : LogicalType::BLOB;
}

//! Bind for the single-argument WKB constructors: picks the executor for the
//! argument type ResolveWkbArgument settled on, and binds a GEOMETRY argument's
//! CRS (see GeometryArgumentCrs).
inline unique_ptr<FunctionData> BindWkbArgument(BindScalarFunctionInput &input, const scalar_function_t &blob_fn,
                                                const scalar_function_t &geometry_fn) {
	auto &bound_function = input.GetBoundFunction();
	const auto &arg_type = bound_function.GetArguments()[0];
	if (arg_type.id() == LogicalTypeId::GEOMETRY) {
		bound_function.SetFunctionCallback(KernelErrorsAsInvalidInput(geometry_fn));
		return GeometryArgumentCrs(arg_type);
	}
	bound_function.SetFunctionCallback(KernelErrorsAsInvalidInput(blob_fn));
	return nullptr;
}

//! A literal or constant-folded expression can produce a non-FLAT_VECTOR at any
//! nesting depth — FlatVector::GetData/IsNull assert genuine flat vectors. A
//! vector carries its own cardinality (a list child's is its total element
//! count across every row), so flattening needs no count.
inline void FlattenIfNeeded(const Vector &vec) {
	if (vec.GetVectorType() != VectorType::FLAT_VECTOR) {
		vec.Flatten();
	}
}

//! Read one row of a geometry_properties STRUCT into the kernel's
//! GeometryMetadata. Resolves `type` and `shells` children by case-insensitive
//! name, applies FlattenIfNeeded at every nesting level before dereferencing,
//! and accepts both HUGEINT face counts (the (BLOB, ANY) bind normalises
//! shells to HUGEINT[][]) and INTEGER face counts, which a producer or a
//! hand-built SQL struct may carry directly. `struct_vec` must already be
//! flattened to `count` rows by the caller. Defined in
//! functions/struct_metadata.cpp.
duckdb_3d::GeometryMetadata ReadGeometryPropertiesStructRow(Vector &struct_vec, idx_t count, idx_t row);

//! The coordinate dimension every v1 value carries. Defined in
//! functions/solid_accessors.cpp; shared because ST_NDims (solid accessors) and
//! ST_CoordDim (geom accessors) live in different translation units.
int32_t CoordinateDimension3D();

//! What duckdb_functions() reports for a function: the only documentation an
//! agent on a SQL connection can reach. One description covers every overload
//! because it carries no parameter_types; parameter_names is the longest
//! overload's list, and shorter overloads take its prefix. `fallible` is false
//! only for a function that never reads its argument, so it cannot raise.
//! DuckDB's optimizer and executor take faster paths for a function that
//! cannot raise (filter pushdown, the dictionary fast path).
struct FunctionDocs {
	vector<string> parameter_names;
	string description;
	string example;
	vector<string> categories;
	bool fallible = true;
};

//! Give every overload's parameters the documented names, so a caller can pass
//! them by name and duckdb_functions() reports them (it reads the signature).
//! Overloads shorter than the list take its prefix.
template <class FUNCTION>
void NameParameters(FUNCTION &function, const vector<string> &names) {
	auto &signature = function.GetSignature();
	for (idx_t i = 0; i < signature.GetParameterCount() && i < names.size(); i++) {
		auto &parameter = signature.GetParameter(i);
		if (parameter.IsVariadic()) {
			continue;
		}
		// A positional-only parameter's name is invisible to a caller; STANDARD
		// takes the argument by position or by name.
		optional<Value> default_value;
		if (parameter.HasDefaultValue()) {
			default_value = *parameter.GetDefaultValue();
		}
		parameter = FunctionParameter(Identifier(names[i]), parameter.GetType(), std::move(default_value),
		                              FunctionParameterKind::STANDARD);
	}
}

//! Register a function set with its documentation attached. Conflicts resolve
//! as the bare RegisterFunction(ScalarFunctionSet) overload resolves them.
//! An overload is fallible unless the docs say otherwise: nearly every one
//! decodes a payload or WKB that can be malformed, and DuckDB turns an error
//! from a non-fallible function into an internal error. Kernel errors reach
//! SQL as InvalidInputException. A bind callback that picks the executor wraps
//! it again, as the callback set here is a placeholder it replaces.
inline void RegisterDocumented(ExtensionLoader &loader, ScalarFunctionSet set, FunctionDocs docs) {
	set.ApplyToFunctions([&](ScalarFunction &function) {
		NameParameters(function, docs.parameter_names);
		if (docs.fallible) {
			function.SetFallible();
			function.SetFunctionCallback(KernelErrorsAsInvalidInput(function.GetFunctionCallback()));
		}
	});
	CreateScalarFunctionInfo info(std::move(set));
	info.on_conflict = OnCreateConflict::ALTER_ON_CONFLICT;
	FunctionDescription description;
	description.parameter_names = std::move(docs.parameter_names);
	description.description = std::move(docs.description);
	description.examples = {std::move(docs.example)};
	description.categories = std::move(docs.categories);
	info.descriptions.push_back(std::move(description));
	loader.RegisterFunction(std::move(info));
}

//! RegisterDocumented for a set whose overloads differ in what an argument
//! position means: one entry per arity, matched to the overloads with that many
//! arguments.
inline void RegisterDocumented(ExtensionLoader &loader, ScalarFunctionSet set, vector<FunctionDocs> docs_by_arity) {
	set.ApplyToFunctions([&](ScalarFunction &function) {
		for (auto &docs : docs_by_arity) {
			if (docs.parameter_names.size() != function.GetSignature().GetParameterCount()) {
				continue;
			}
			NameParameters(function, docs.parameter_names);
			if (docs.fallible) {
				function.SetFallible();
				function.SetFunctionCallback(KernelErrorsAsInvalidInput(function.GetFunctionCallback()));
			}
		}
	});
	CreateScalarFunctionInfo info(std::move(set));
	info.on_conflict = OnCreateConflict::ALTER_ON_CONFLICT;
	for (auto &docs : docs_by_arity) {
		FunctionDescription description;
		description.parameter_types = vector<LogicalType>(docs.parameter_names.size(), LogicalType::ANY);
		description.parameter_names = std::move(docs.parameter_names);
		description.description = std::move(docs.description);
		description.examples = {std::move(docs.example)};
		description.categories = std::move(docs.categories);
		info.descriptions.push_back(std::move(description));
	}
	loader.RegisterFunction(std::move(info));
}

//! Single-overload form of RegisterDocumented.
inline void RegisterDocumented(ExtensionLoader &loader, ScalarFunction function, FunctionDocs docs) {
	ScalarFunctionSet set(function.GetName());
	set.AddFunction(std::move(function));
	RegisterDocumented(loader, std::move(set), std::move(docs));
}

//! Per-domain registration hooks, called from LoadInternal.
void RegisterFixtureFunctions(ExtensionLoader &loader);
void RegisterSolidIOFunctions(ExtensionLoader &loader, const LogicalType &solid_3d_type);
void RegisterSolidAccessorFunctions(ExtensionLoader &loader, const LogicalType &solid_3d_type,
                                    const LogicalType &geom_3d_type);
void RegisterGeomAccessorFunctions(ExtensionLoader &loader, const LogicalType &solid_3d_type,
                                   const LogicalType &geom_3d_type);
void RegisterDistanceFunctions(ExtensionLoader &loader, const LogicalType &solid_3d_type,
                               const LogicalType &geom_3d_type);
void RegisterPayloadCasts(ExtensionLoader &loader, const LogicalType &solid_3d_type, const LogicalType &geom_3d_type);
void RegisterBoundsFunctions(ExtensionLoader &loader, const LogicalType &solid_3d_type,
                             const LogicalType &geom_3d_type);
void RegisterFaceFunctions(ExtensionLoader &loader, const LogicalType &solid_3d_type, const LogicalType &geom_3d_type);
void RegisterCrsFunctions(ExtensionLoader &loader, const LogicalType &solid_3d_type, const LogicalType &geom_3d_type);
void RegisterTransformFunctions(ExtensionLoader &loader, const LogicalType &solid_3d_type,
                                const LogicalType &geom_3d_type);

} // namespace duckdb
