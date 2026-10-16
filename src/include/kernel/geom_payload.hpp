#pragma once

#include "kernel/geom_model.hpp"
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace duckdb_3d {

//! Magic bytes for the GEOM_3D binary format: "D3DG".
constexpr uint8_t GEOM_PAYLOAD_MAGIC[4] = {'D', '3', 'D', 'G'};
//! Version 2 added the CRS and the triangulation cache; version 1 payloads are
//! refused.
constexpr uint16_t GEOM_PAYLOAD_VERSION_MAJOR = 2;
constexpr uint16_t GEOM_PAYLOAD_VERSION_MINOR = 0;

//! Serialize a GeomModel into the GEOM_3D binary payload. A surface-class model
//! whose triangulation cache is not filled is triangulated here, so every
//! GEOM_3D value carries its triangles; that throws std::invalid_argument for a
//! polygon that admits no tiling (TriangulateGeomModel).
std::vector<uint8_t> SerializeGeomPayload(const GeomModel &model);

//! Deserialize a GEOM_3D binary payload into a GeomModel.
//! Throws std::runtime_error on invalid magic/version or truncated data.
GeomModel DeserializeGeomPayload(const uint8_t *data, size_t size);

//! Re-derive what a GEOM_3D payload caches (bounding box, triangulation) from
//! its vertices and offsets, and serialize the result. For payloads from
//! outside the extension, whose caches cannot be trusted. Throws what
//! DeserializeGeomPayload and SerializeGeomPayload throw.
std::vector<uint8_t> RebuildGeomPayload(const uint8_t *data, size_t size);

//! Lightweight view of a GEOM_3D payload: geometry type, vertex count, and
//! bounding box — all stored in the fixed front header. Reading this is O(1)
//! and avoids materialising vertices/rings/parts.
struct GeomPayloadInfo {
	GeomType type;
	uint32_t vertex_count;
	BBox3D bbox;
	std::string_view crs; //!< into the payload: the header is read per row, and a copy costs more than the rest
};

//! Read only the front header (type + bbox + vertex count) of a GEOM_3D payload,
//! without deserialising the body. Throws on invalid magic or unsupported major
//! version. Use for bounds/ZMin/ZMax accessors on GEOM_3D values.
GeomPayloadInfo ReadGeomPayloadHeader(const uint8_t *data, size_t size);

//! The GEOM_3D payload with its CRS replaced by `crs` (empty clears it); the
//! body is copied, not decoded. Throws what ReadGeomPayloadHeader throws.
std::vector<uint8_t> WithGeomPayloadCrs(const uint8_t *data, size_t size, const std::string &crs);

} // namespace duckdb_3d
