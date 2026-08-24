#pragma once

#include "duckdb.hpp"
#include "kernel/geom_payload.hpp"
#include "kernel/metadata_parser.hpp"
#include "kernel/payload.hpp"

#include <cstring>

namespace duckdb {

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

//! Bind for the single-argument WKB constructors. The argument is declared ANY
//! so that ONE candidate covers both the BLOB a plain column carries and the
//! GEOMETRY a Parquet-annotated column carries. Two separate overloads would
//! read more naturally but make an untyped `ST_3DFromWKB(NULL)` ambiguous —
//! SQLNULL casts to either at the same cost — and that call has always bound.
inline unique_ptr<FunctionData> BindWkbArgument(ScalarFunction &bound_function,
                                                vector<unique_ptr<Expression>> &arguments, scalar_function_t blob_fn,
                                                scalar_function_t geometry_fn) {
	auto &arg_type = arguments[0]->return_type;
	if (arg_type.id() == LogicalTypeId::UNKNOWN) {
		// A prepared-statement '?' parameter: defer to a later re-bind.
		throw ParameterNotResolvedException();
	}
	if (arg_type.id() == LogicalTypeId::GEOMETRY) {
		// Keep the argument's own type, CRS parameter and all, so no cast is added.
		bound_function.arguments[0] = arg_type;
		bound_function.function = std::move(geometry_fn);
		return nullptr;
	}
	bound_function.arguments[0] = LogicalType::BLOB;
	bound_function.function = std::move(blob_fn);
	return nullptr;
}

//! A literal or constant-folded expression can produce a non-FLAT_VECTOR at any
//! nesting depth — FlatVector::GetData/IsNull assert genuine flat vectors.
//! `count` is this level's own cardinality (a list child's total element count
//! across every row, not the outer chunk's row count).
inline void FlattenIfNeeded(Vector &vec, idx_t count) {
	if (vec.GetVectorType() != VectorType::FLAT_VECTOR) {
		vec.Flatten(count);
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

//! Per-domain registration hooks, called from LoadInternal.
void RegisterFixtureFunctions(ExtensionLoader &loader);
void RegisterSolidIOFunctions(ExtensionLoader &loader, const LogicalType &solid_3d_type);
void RegisterSolidAccessorFunctions(ExtensionLoader &loader, const LogicalType &solid_3d_type,
                                    const LogicalType &geom_3d_type);
void RegisterGeomAccessorFunctions(ExtensionLoader &loader, const LogicalType &solid_3d_type,
                                   const LogicalType &geom_3d_type);
void RegisterDistanceFunctions(ExtensionLoader &loader, const LogicalType &geom_3d_type);
void RegisterTransformFunctions(ExtensionLoader &loader, const LogicalType &solid_3d_type,
                                const LogicalType &geom_3d_type);

} // namespace duckdb
