#include "kernel/geom_payload.hpp"
#include "kernel/byte_io.hpp"

#include <cstring>
#include <stdexcept>

namespace duckdb_3d {

namespace {

constexpr const char *kTruncationMessage = "DeserializeGeomPayload: truncated payload";

//! Structural validation mirroring the SOLID_3D path's ValidatePayloadModel
//! (payload.cpp): ring_offsets/part_offsets are used as raw indices by every
//! GEOM_3D reader (length, distance decomposition, footprint, WKT/GeoJSON/WKB
//! writers), so a crafted payload must be rejected here, not deep inside a
//! reader. The per-type shapes match what ParseGeomWKB emits.
void ValidateGeomModel(const GeomModel &model) {
	auto vertex_count = static_cast<uint32_t>(model.vertices.size());
	switch (model.type) {
	case GeomType::Point:
	case GeomType::LineString:
		if (!model.ring_offsets.empty() || !model.part_offsets.empty()) {
			throw std::runtime_error("GEOM_3D payload: Point/LineString must not carry offsets");
		}
		break;
	case GeomType::Polygon:
		if (!model.part_offsets.empty()) {
			throw std::runtime_error("GEOM_3D payload: Polygon must not carry part offsets");
		}
		if (!model.ring_offsets.empty()) {
			ValidateOffsets(model.ring_offsets, vertex_count, "ring-vertex", "GEOM_3D payload");
		}
		break;
	case GeomType::MultiPoint:
	case GeomType::MultiLineString:
		if (!model.ring_offsets.empty()) {
			throw std::runtime_error("GEOM_3D payload: MultiPoint/MultiLineString must not carry ring offsets");
		}
		if (!model.part_offsets.empty()) {
			ValidateOffsets(model.part_offsets, vertex_count, "part-vertex", "GEOM_3D payload");
		}
		break;
	case GeomType::MultiPolygon:
	case GeomType::PolyhedralSurface:
		if (!model.ring_offsets.empty()) {
			ValidateOffsets(model.ring_offsets, vertex_count, "ring-vertex", "GEOM_3D payload");
		}
		if (!model.part_offsets.empty()) {
			// part_offsets holds ring indices; writers dereference
			// ring_offsets[part_offsets[k] + 1], so rings must be fully partitioned.
			if (model.ring_offsets.empty()) {
				throw std::runtime_error("GEOM_3D payload: part offsets without ring offsets");
			}
			ValidateOffsets(model.part_offsets, static_cast<uint32_t>(model.ring_offsets.size()) - 1, "part-ring",
			                "GEOM_3D payload");
		}
		break;
	case GeomType::GeometryCollection:
		// Not yet produced by ParseGeomWKB; accept only offset-free payloads so a
		// crafted collection cannot smuggle unchecked indices past validation.
		if (!model.ring_offsets.empty() || !model.part_offsets.empty()) {
			throw std::runtime_error("GEOM_3D payload: GeometryCollection offsets are not supported");
		}
		break;
	default:
		throw std::runtime_error("GEOM_3D payload: unknown geometry type code");
	}
}

} // namespace

std::vector<uint8_t> SerializeGeomPayload(const GeomModel &model) {
	ByteWriter w;
	w.WriteBytes(GEOM_PAYLOAD_MAGIC, 4);
	w.WriteU16(GEOM_PAYLOAD_VERSION_MAJOR);
	w.WriteU16(GEOM_PAYLOAD_VERSION_MINOR);
	w.WriteU32(static_cast<uint32_t>(model.type));

	w.WriteF64(model.bbox.min_x);
	w.WriteF64(model.bbox.min_y);
	w.WriteF64(model.bbox.min_z);
	w.WriteF64(model.bbox.max_x);
	w.WriteF64(model.bbox.max_y);
	w.WriteF64(model.bbox.max_z);

	w.WriteU32(static_cast<uint32_t>(model.vertices.size()));
	for (const auto &v : model.vertices) {
		w.WriteF64(v.x);
		w.WriteF64(v.y);
		w.WriteF64(v.z);
	}
	w.WriteU32(static_cast<uint32_t>(model.ring_offsets.size()));
	for (auto o : model.ring_offsets) {
		w.WriteU32(o);
	}
	w.WriteU32(static_cast<uint32_t>(model.part_offsets.size()));
	for (auto o : model.part_offsets) {
		w.WriteU32(o);
	}
	return w.buffer;
}

GeomPayloadInfo ReadGeomPayloadHeader(const uint8_t *data, size_t size) {
	ByteReader r(data, size, kTruncationMessage);
	r.Require(4);
	if (std::memcmp(data, GEOM_PAYLOAD_MAGIC, 4) != 0) {
		throw std::runtime_error("ReadGeomPayloadHeader: bad magic (not a GEOM_3D value)");
	}
	r.pos = 4;
	uint16_t major = r.ReadU16();
	r.ReadU16(); // minor
	if (major != GEOM_PAYLOAD_VERSION_MAJOR) {
		throw std::runtime_error("ReadGeomPayloadHeader: unsupported major version");
	}

	GeomPayloadInfo info;
	info.type = static_cast<GeomType>(r.ReadU32());
	info.bbox.min_x = r.ReadF64();
	info.bbox.min_y = r.ReadF64();
	info.bbox.min_z = r.ReadF64();
	info.bbox.max_x = r.ReadF64();
	info.bbox.max_y = r.ReadF64();
	info.bbox.max_z = r.ReadF64();
	info.vertex_count = r.ReadU32();
	return info;
}

GeomModel DeserializeGeomPayload(const uint8_t *data, size_t size) {
	ByteReader r(data, size, kTruncationMessage);
	r.Require(4);
	if (std::memcmp(data, GEOM_PAYLOAD_MAGIC, 4) != 0) {
		throw std::runtime_error("DeserializeGeomPayload: bad magic (not a GEOM_3D value)");
	}
	r.pos = 4;
	uint16_t major = r.ReadU16();
	r.ReadU16(); // minor
	if (major != GEOM_PAYLOAD_VERSION_MAJOR) {
		throw std::runtime_error("DeserializeGeomPayload: unsupported major version");
	}

	GeomModel model;
	model.type = static_cast<GeomType>(r.ReadU32());

	model.bbox.min_x = r.ReadF64();
	model.bbox.min_y = r.ReadF64();
	model.bbox.min_z = r.ReadF64();
	model.bbox.max_x = r.ReadF64();
	model.bbox.max_y = r.ReadF64();
	model.bbox.max_z = r.ReadF64();

	uint32_t vertex_count = r.ReadU32();
	r.RequireCount(vertex_count, 3 * sizeof(double), "vertex");
	model.vertices.reserve(vertex_count);
	for (uint32_t i = 0; i < vertex_count; i++) {
		Vertex3D v;
		v.x = r.ReadF64();
		v.y = r.ReadF64();
		v.z = r.ReadF64();
		model.vertices.push_back(v);
	}
	uint32_t ring_off_count = r.ReadU32();
	r.RequireCount(ring_off_count, sizeof(uint32_t), "ring-offset");
	model.ring_offsets.reserve(ring_off_count);
	for (uint32_t i = 0; i < ring_off_count; i++) {
		model.ring_offsets.push_back(r.ReadU32());
	}
	uint32_t part_off_count = r.ReadU32();
	r.RequireCount(part_off_count, sizeof(uint32_t), "part-offset");
	model.part_offsets.reserve(part_off_count);
	for (uint32_t i = 0; i < part_off_count; i++) {
		model.part_offsets.push_back(r.ReadU32());
	}
	ValidateGeomModel(model);
	return model;
}

} // namespace duckdb_3d
