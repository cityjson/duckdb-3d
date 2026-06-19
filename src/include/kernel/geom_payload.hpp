#pragma once

#include "kernel/geom_model.hpp"
#include <cstddef>
#include <cstdint>
#include <vector>

namespace duckdb_3d {

//! Magic bytes for the GEOM_3D binary format: "D3DG".
constexpr uint8_t GEOM_PAYLOAD_MAGIC[4] = {'D', '3', 'D', 'G'};
constexpr uint16_t GEOM_PAYLOAD_VERSION_MAJOR = 1;
constexpr uint16_t GEOM_PAYLOAD_VERSION_MINOR = 0;

//! Serialize a GeomModel into the GEOM_3D binary payload.
std::vector<uint8_t> SerializeGeomPayload(const GeomModel &model);

//! Deserialize a GEOM_3D binary payload into a GeomModel.
//! Throws std::runtime_error on invalid magic/version or truncated data.
GeomModel DeserializeGeomPayload(const uint8_t *data, size_t size);

//! Lightweight view of a GEOM_3D payload: geometry type, vertex count, and
//! bounding box — all stored in the fixed front header. Reading this is O(1)
//! and avoids materialising vertices/rings/parts.
struct GeomPayloadInfo {
	GeomType type;
	uint32_t vertex_count;
	BBox3D bbox;
};

//! Read only the front header (type + bbox + vertex count) of a GEOM_3D payload,
//! without deserialising the body. Throws on invalid magic or unsupported major
//! version. Use for bounds/ZMin/ZMax accessors on GEOM_3D values.
GeomPayloadInfo ReadGeomPayloadHeader(const uint8_t *data, size_t size);

} // namespace duckdb_3d
