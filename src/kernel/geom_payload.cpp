#include "kernel/geom_payload.hpp"

#include <cstring>
#include <stdexcept>

namespace duckdb_3d {

namespace {

void PushU16(std::vector<uint8_t> &buf, uint16_t v) {
	buf.push_back(v & 0xFF);
	buf.push_back((v >> 8) & 0xFF);
}
void PushU32(std::vector<uint8_t> &buf, uint32_t v) {
	buf.push_back(v & 0xFF);
	buf.push_back((v >> 8) & 0xFF);
	buf.push_back((v >> 16) & 0xFF);
	buf.push_back((v >> 24) & 0xFF);
}
void PushF64(std::vector<uint8_t> &buf, double v) {
	uint8_t b[8];
	std::memcpy(b, &v, 8);
	buf.insert(buf.end(), b, b + 8);
}

struct Reader {
	const uint8_t *data;
	size_t size;
	size_t pos;

	void Require(size_t n) const {
		if (pos + n > size) {
			throw std::runtime_error("DeserializeGeomPayload: truncated payload");
		}
	}
	//! Reject a declared element count before reserving for it: the elements
	//! cannot fit in the bytes that remain. Guards against a malformed header
	//! claiming a huge count, which would otherwise reserve a large buffer
	//! before the truncated read is detected. `elem_size` is 64-bit so the
	//! multiplication cannot overflow.
	void RequireCount(uint64_t count, uint64_t elem_size, const char *what) const {
		if (count * elem_size > size - pos) {
			throw std::runtime_error(std::string("DeserializeGeomPayload: declared ") + what +
			                         " count exceeds remaining payload size");
		}
	}
	uint16_t U16() {
		Require(2);
		uint16_t v = data[pos] | (uint16_t(data[pos + 1]) << 8);
		pos += 2;
		return v;
	}
	uint32_t U32() {
		Require(4);
		uint32_t v;
		std::memcpy(&v, data + pos, 4);
		pos += 4;
		return v;
	}
	double F64() {
		Require(8);
		double v;
		std::memcpy(&v, data + pos, 8);
		pos += 8;
		return v;
	}
};

} // namespace

std::vector<uint8_t> SerializeGeomPayload(const GeomModel &model) {
	std::vector<uint8_t> buf;
	buf.insert(buf.end(), GEOM_PAYLOAD_MAGIC, GEOM_PAYLOAD_MAGIC + 4);
	PushU16(buf, GEOM_PAYLOAD_VERSION_MAJOR);
	PushU16(buf, GEOM_PAYLOAD_VERSION_MINOR);
	PushU32(buf, static_cast<uint32_t>(model.type));

	PushF64(buf, model.bbox.min_x);
	PushF64(buf, model.bbox.min_y);
	PushF64(buf, model.bbox.min_z);
	PushF64(buf, model.bbox.max_x);
	PushF64(buf, model.bbox.max_y);
	PushF64(buf, model.bbox.max_z);

	PushU32(buf, static_cast<uint32_t>(model.vertices.size()));
	for (const auto &v : model.vertices) {
		PushF64(buf, v.x);
		PushF64(buf, v.y);
		PushF64(buf, v.z);
	}
	PushU32(buf, static_cast<uint32_t>(model.ring_offsets.size()));
	for (auto o : model.ring_offsets) {
		PushU32(buf, o);
	}
	PushU32(buf, static_cast<uint32_t>(model.part_offsets.size()));
	for (auto o : model.part_offsets) {
		PushU32(buf, o);
	}
	return buf;
}

GeomPayloadInfo ReadGeomPayloadHeader(const uint8_t *data, size_t size) {
	Reader r{data, size, 0};
	r.Require(4);
	if (std::memcmp(data, GEOM_PAYLOAD_MAGIC, 4) != 0) {
		throw std::runtime_error("ReadGeomPayloadHeader: bad magic (not a GEOM_3D value)");
	}
	r.pos = 4;
	uint16_t major = r.U16();
	r.U16(); // minor
	if (major != GEOM_PAYLOAD_VERSION_MAJOR) {
		throw std::runtime_error("ReadGeomPayloadHeader: unsupported major version");
	}

	GeomPayloadInfo info;
	info.type = static_cast<GeomType>(r.U32());
	info.bbox.min_x = r.F64();
	info.bbox.min_y = r.F64();
	info.bbox.min_z = r.F64();
	info.bbox.max_x = r.F64();
	info.bbox.max_y = r.F64();
	info.bbox.max_z = r.F64();
	info.vertex_count = r.U32();
	return info;
}

GeomModel DeserializeGeomPayload(const uint8_t *data, size_t size) {
	Reader r{data, size, 0};
	r.Require(4);
	if (std::memcmp(data, GEOM_PAYLOAD_MAGIC, 4) != 0) {
		throw std::runtime_error("DeserializeGeomPayload: bad magic (not a GEOM_3D value)");
	}
	r.pos = 4;
	uint16_t major = r.U16();
	r.U16(); // minor
	if (major != GEOM_PAYLOAD_VERSION_MAJOR) {
		throw std::runtime_error("DeserializeGeomPayload: unsupported major version");
	}

	GeomModel model;
	model.type = static_cast<GeomType>(r.U32());

	model.bbox.min_x = r.F64();
	model.bbox.min_y = r.F64();
	model.bbox.min_z = r.F64();
	model.bbox.max_x = r.F64();
	model.bbox.max_y = r.F64();
	model.bbox.max_z = r.F64();

	uint32_t vertex_count = r.U32();
	r.RequireCount(vertex_count, 3 * sizeof(double), "vertex");
	model.vertices.reserve(vertex_count);
	for (uint32_t i = 0; i < vertex_count; i++) {
		Vertex3D v;
		v.x = r.F64();
		v.y = r.F64();
		v.z = r.F64();
		model.vertices.push_back(v);
	}
	uint32_t ring_off_count = r.U32();
	r.RequireCount(ring_off_count, sizeof(uint32_t), "ring-offset");
	model.ring_offsets.reserve(ring_off_count);
	for (uint32_t i = 0; i < ring_off_count; i++) {
		model.ring_offsets.push_back(r.U32());
	}
	uint32_t part_off_count = r.U32();
	r.RequireCount(part_off_count, sizeof(uint32_t), "part-offset");
	model.part_offsets.reserve(part_off_count);
	for (uint32_t i = 0; i < part_off_count; i++) {
		model.part_offsets.push_back(r.U32());
	}
	return model;
}

} // namespace duckdb_3d
