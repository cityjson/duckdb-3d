#pragma once

#include "kernel/solid_model.hpp"
#include <cstdint>
#include <vector>
#include <string>
#include <stdexcept>

namespace duckdb_3d {

//! Magic bytes for SOLID_3D binary format: "D3DS"
constexpr uint8_t PAYLOAD_MAGIC[4] = {'D', '3', 'D', 'S'};

//! v1 format version
constexpr uint16_t PAYLOAD_VERSION_MAJOR = 1;
constexpr uint16_t PAYLOAD_VERSION_MINOR = 0;

//! Binary payload header (fixed size)
struct PayloadHeader {
	uint8_t magic[4];       // "D3DS"
	uint16_t version_major; // 1
	uint16_t version_minor; // 0
	uint32_t flags;         // reserved
	uint32_t vertex_count;
	uint32_t solid_count;
	uint32_t shell_count;
	uint32_t face_count;
	uint32_t ring_count;
	uint32_t triangle_count;
	// bbox: 6 x f64
	double bbox_min_x, bbox_min_y, bbox_min_z;
	double bbox_max_x, bbox_max_y, bbox_max_z;
};

//! Serialize a SolidModel into the SOLID_3D binary payload format.
//! Returns the raw bytes of the payload.
std::vector<uint8_t> SerializePayload(const SolidModel &model);

//! Deserialize a SOLID_3D binary payload into a SolidModel.
//! Throws std::runtime_error on invalid or unsupported payloads.
SolidModel DeserializePayload(const uint8_t *data, size_t size);

} // namespace duckdb_3d
