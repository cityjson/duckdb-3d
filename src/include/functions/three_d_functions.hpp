#pragma once

#include "duckdb.hpp"
#include "kernel/geom_payload.hpp"
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

//! A literal or constant-folded expression can produce a non-FLAT_VECTOR at any
//! nesting depth — FlatVector::GetData/IsNull assert genuine flat vectors.
//! `count` is this level's own cardinality (a list child's total element count
//! across every row, not the outer chunk's row count).
inline void FlattenIfNeeded(Vector &vec, idx_t count) {
	if (vec.GetVectorType() != VectorType::FLAT_VECTOR) {
		vec.Flatten(count);
	}
}

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
void RegisterArrowNativeFunctions(ExtensionLoader &loader, const LogicalType &solid_3d_type,
                                  const LogicalType &geom_3d_type);

} // namespace duckdb
