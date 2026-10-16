#include "functions/three_d_functions.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/operator/cast_operators.hpp"
#include "duckdb/common/vector_operations/vector_operations.hpp"
#include "duckdb/function/cast/default_casts.hpp"

#include "kernel/geom_payload.hpp"
#include "kernel/model_builder.hpp"

#include <exception>
#include <new>
#include <string>
#include <vector>

namespace duckdb {

// ──────────────────────────────────────────────────────────────
// BLOB -> SOLID_3D / GEOM_3D
// ──────────────────────────────────────────────────────────────
// A payload stored as a plain BLOB (a Parquet column, a table written by
// another tool) is untrusted: its cached triangulation, bounding box and
// validation verdict decide what the functions return, and nothing guarantees
// they match its geometry. The casts are the only way such a value becomes a
// SOLID_3D or GEOM_3D, and they re-derive every cache from the topology and
// vertices. They are explicit: the rebuild costs a triangulation and, for a
// solid, a validation, so a query materialises the cast once rather than paying
// for it in every function call.
template <PayloadKind KIND>
static bool CastBlobToPayload(Vector &source, Vector &result, idx_t count, CastParameters &parameters) {
	bool all_converted = true;
	UnaryExecutor::ExecuteWithNulls<string_t, string_t>(
	    source, result, count, [&](string_t blob, ValidityMask &mask, idx_t idx) {
		    auto data = reinterpret_cast<const uint8_t *>(blob.GetData());
		    auto size = blob.GetSize();
		    const char *type_name = KIND == PayloadKind::Solid ? "SOLID_3D" : "GEOM_3D";
		    std::string error;
		    if (GetPayloadKind(data, size) != KIND) {
			    error = std::string("BLOB is not a ") + type_name + " payload; build one from WKB with " +
			            (KIND == PayloadKind::Solid ? "ST_3DFromWKB" : "ST_Geom3DFromWKB");
		    } else {
			    try {
				    auto bytes = KIND == PayloadKind::Solid ? duckdb_3d::RebuildSolidPayload(data, size)
				                                            : duckdb_3d::RebuildGeomPayload(data, size);
				    return StringVector::AddStringOrBlob(result, reinterpret_cast<const char *>(bytes.data()),
				                                         bytes.size());
			    } catch (std::bad_alloc &) {
				    throw; // out of memory is not a property of the row
			    } catch (std::exception &e) {
				    error = std::string("Cannot cast BLOB to ") + type_name + ": " + e.what();
			    }
		    }
		    HandleCastError::AssignError(error, parameters);
		    all_converted = false;
		    mask.SetInvalid(idx);
		    return string_t();
	    });
	return all_converted;
}

//! VARCHAR -> SOLID_3D / GEOM_3D: the string's bytes (with DuckDB's BLOB
//! escapes) through the BLOB cast. Without it the default VARCHAR -> BLOB cast
//! would hand a crafted string to the alias untouched.
template <PayloadKind KIND>
static bool CastVarcharToPayload(Vector &source, Vector &result, idx_t count, CastParameters &parameters) {
	// Under TRY_CAST the first stage NULLs the rows that are no BLOB and goes
	// on; the second must still run on the rest, so neither returns early.
	Vector blob(LogicalType::BLOB, count);
	bool all_blobs = VectorOperations::DefaultTryCast(source, blob, count, parameters.error_message, parameters.strict);
	bool all_payloads = CastBlobToPayload<KIND>(blob, result, count, parameters);
	return all_blobs && all_payloads;
}

void RegisterPayloadCasts(ExtensionLoader &loader, const LogicalType &solid_3d_type, const LogicalType &geom_3d_type) {
	loader.RegisterCastFunction(LogicalType::BLOB, solid_3d_type, CastBlobToPayload<PayloadKind::Solid>);
	loader.RegisterCastFunction(LogicalType::BLOB, geom_3d_type, CastBlobToPayload<PayloadKind::Geom>);
	loader.RegisterCastFunction(LogicalType::VARCHAR, solid_3d_type, CastVarcharToPayload<PayloadKind::Solid>);
	loader.RegisterCastFunction(LogicalType::VARCHAR, geom_3d_type, CastVarcharToPayload<PayloadKind::Geom>);
	// The payload bytes are the BLOB: casting back is free and trusts nothing.
	loader.RegisterCastFunction(solid_3d_type, LogicalType::BLOB, DefaultCasts::ReinterpretCast);
	loader.RegisterCastFunction(geom_3d_type, LogicalType::BLOB, DefaultCasts::ReinterpretCast);
}

} // namespace duckdb
