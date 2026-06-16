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

} // namespace duckdb_3d
