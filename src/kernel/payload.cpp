#include "kernel/payload.hpp"
#include <cstring>
#include <stdexcept>

namespace duckdb_3d {

namespace {

//! Helper to write raw bytes into a buffer
class PayloadWriter {
public:
	std::vector<uint8_t> buffer;

	void WriteBytes(const void *src, size_t len) {
		auto *bytes = static_cast<const uint8_t *>(src);
		buffer.insert(buffer.end(), bytes, bytes + len);
	}

	template <typename T>
	void Write(T value) {
		WriteBytes(&value, sizeof(T));
	}

	void WriteArray(const uint32_t *data, size_t count) {
		WriteBytes(data, count * sizeof(uint32_t));
	}
};

//! Helper to read raw bytes from a buffer
class PayloadReader {
public:
	const uint8_t *data;
	size_t size;
	size_t pos = 0;

	PayloadReader(const uint8_t *data, size_t size) : data(data), size(size) {
	}

	void ReadBytes(void *dst, size_t len) {
		if (pos + len > size) {
			throw std::runtime_error("SOLID_3D payload truncated");
		}
		std::memcpy(dst, data + pos, len);
		pos += len;
	}

	template <typename T>
	T Read() {
		T value;
		ReadBytes(&value, sizeof(T));
		return value;
	}

	void ReadArray(uint32_t *dst, size_t count) {
		ReadBytes(dst, count * sizeof(uint32_t));
	}
};

void ValidateOffsets(const std::vector<uint32_t> &offsets, uint32_t expected_last, const char *name) {
	if (offsets.empty()) {
		throw std::runtime_error(std::string("SOLID_3D payload: missing ") + name + " offsets");
	}
	if (offsets.front() != 0) {
		throw std::runtime_error(std::string("SOLID_3D payload: invalid ") + name + " offsets");
	}
	for (size_t i = 1; i < offsets.size(); i++) {
		if (offsets[i] < offsets[i - 1]) {
			throw std::runtime_error(std::string("SOLID_3D payload: non-monotonic ") + name + " offsets");
		}
	}
	if (offsets.back() != expected_last) {
		throw std::runtime_error(std::string("SOLID_3D payload: inconsistent ") + name + " offsets");
	}
}

void ValidatePayloadModel(const SolidModel &model, uint32_t vertex_count, uint32_t solid_count, uint32_t shell_count,
                          uint32_t face_count, uint32_t ring_count, uint32_t triangle_count) {
	ValidateOffsets(model.solid_shell_offsets, shell_count, "solid-shell");
	ValidateOffsets(model.shell_face_offsets, face_count, "shell-face");
	ValidateOffsets(model.face_ring_offsets, ring_count, "face-ring");
	ValidateOffsets(model.face_triangle_offsets, triangle_count, "face-triangle");

	if (model.ring_vertex_offsets.empty() || model.ring_vertex_offsets.front() != 0) {
		throw std::runtime_error("SOLID_3D payload: invalid ring-vertex offsets");
	}
	for (size_t i = 1; i < model.ring_vertex_offsets.size(); i++) {
		if (model.ring_vertex_offsets[i] < model.ring_vertex_offsets[i - 1]) {
			throw std::runtime_error("SOLID_3D payload: non-monotonic ring-vertex offsets");
		}
	}

	if (model.vertices.size() != vertex_count || model.SolidCount() != solid_count || model.ShellCount() != shell_count ||
	    model.FaceCount() != face_count || model.RingCount() != ring_count ||
	    model.TriangleCount() != triangle_count) {
		throw std::runtime_error("SOLID_3D payload: header counts do not match payload body");
	}

	for (uint32_t i = 0; i < solid_count; i++) {
		if (model.solid_shell_offsets[i] == model.solid_shell_offsets[i + 1]) {
			throw std::runtime_error("SOLID_3D payload: solids must contain at least one shell");
		}
	}
	for (uint32_t i = 0; i < shell_count; i++) {
		if (model.shell_face_offsets[i] == model.shell_face_offsets[i + 1]) {
			throw std::runtime_error("SOLID_3D payload: shells must contain at least one face");
		}
	}
	for (uint32_t i = 0; i < face_count; i++) {
		if (model.face_ring_offsets[i] == model.face_ring_offsets[i + 1]) {
			throw std::runtime_error("SOLID_3D payload: faces must contain at least one ring");
		}
	}

	for (uint32_t idx : model.ring_vertex_indices) {
		if (idx >= vertex_count) {
			throw std::runtime_error("SOLID_3D payload: ring vertex index out of range");
		}
	}
	for (uint32_t idx : model.triangle_vertex_indices) {
		if (idx >= vertex_count) {
			throw std::runtime_error("SOLID_3D payload: triangle vertex index out of range");
		}
	}
}

} // anonymous namespace

std::vector<uint8_t> SerializePayload(const SolidModel &model) {
	PayloadWriter writer;

	// Header
	writer.WriteBytes(PAYLOAD_MAGIC, 4);
	writer.Write<uint16_t>(PAYLOAD_VERSION_MAJOR);
	writer.Write<uint16_t>(PAYLOAD_VERSION_MINOR);
	writer.Write<uint32_t>(0); // flags

	uint32_t vertex_count = static_cast<uint32_t>(model.vertices.size());
	uint32_t solid_count = model.SolidCount();
	uint32_t shell_count = model.ShellCount();
	uint32_t face_count = model.FaceCount();
	uint32_t ring_count = model.RingCount();
	uint32_t triangle_count = model.TriangleCount();

	writer.Write<uint32_t>(vertex_count);
	writer.Write<uint32_t>(solid_count);
	writer.Write<uint32_t>(shell_count);
	writer.Write<uint32_t>(face_count);
	writer.Write<uint32_t>(ring_count);
	writer.Write<uint32_t>(triangle_count);

	// BBox
	writer.Write<double>(model.bbox.min_x);
	writer.Write<double>(model.bbox.min_y);
	writer.Write<double>(model.bbox.min_z);
	writer.Write<double>(model.bbox.max_x);
	writer.Write<double>(model.bbox.max_y);
	writer.Write<double>(model.bbox.max_z);

	// Offset arrays
	writer.WriteArray(model.solid_shell_offsets.data(), solid_count + 1);
	writer.WriteArray(model.shell_face_offsets.data(), shell_count + 1);
	writer.WriteArray(model.face_ring_offsets.data(), face_count + 1);
	writer.WriteArray(model.ring_vertex_offsets.data(), ring_count + 1);
	writer.WriteArray(model.face_triangle_offsets.data(), face_count + 1);

	// Data arrays: vertices
	for (uint32_t i = 0; i < vertex_count; i++) {
		writer.Write<double>(model.vertices[i].x);
		writer.Write<double>(model.vertices[i].y);
		writer.Write<double>(model.vertices[i].z);
	}

	// Data arrays: ring vertex indices
	writer.WriteArray(model.ring_vertex_indices.data(), model.ring_vertex_indices.size());

	// Data arrays: triangle vertex indices
	writer.WriteArray(model.triangle_vertex_indices.data(), triangle_count * 3);

	// Validation cache
	writer.Write<uint32_t>(model.validation.open_edge_count);
	writer.Write<uint32_t>(model.validation.non_manifold_edge_count);
	writer.Write<uint32_t>(model.validation.degenerate_face_count);
	writer.Write<uint32_t>(model.validation.orientation_error_count);

	uint32_t summary_flags = 0;
	if (model.validation.is_closed) {
		summary_flags |= 0x01;
	}
	if (model.validation.is_manifold) {
		summary_flags |= 0x02;
	}
	if (model.validation.is_oriented) {
		summary_flags |= 0x04;
	}
	if (model.validation.is_valid) {
		summary_flags |= 0x08;
	}
	writer.Write<uint32_t>(summary_flags);

	return writer.buffer;
}

SolidModel DeserializePayload(const uint8_t *data, size_t size) {
	PayloadReader reader(data, size);

	// Verify magic
	uint8_t magic[4];
	reader.ReadBytes(magic, 4);
	if (std::memcmp(magic, PAYLOAD_MAGIC, 4) != 0) {
		throw std::runtime_error("SOLID_3D payload: invalid magic bytes");
	}

	// Verify version
	uint16_t major = reader.Read<uint16_t>();
	uint16_t minor = reader.Read<uint16_t>();
	if (major != PAYLOAD_VERSION_MAJOR) {
		throw std::runtime_error("SOLID_3D payload: unsupported major version " + std::to_string(major));
	}
	// Minor version: accept if >= our minor (forward compat for minor additions)

	uint32_t flags = reader.Read<uint32_t>();
	(void)flags; // reserved

	uint32_t vertex_count = reader.Read<uint32_t>();
	uint32_t solid_count = reader.Read<uint32_t>();
	uint32_t shell_count = reader.Read<uint32_t>();
	uint32_t face_count = reader.Read<uint32_t>();
	uint32_t ring_count = reader.Read<uint32_t>();
	uint32_t triangle_count = reader.Read<uint32_t>();

	SolidModel model;

	// BBox
	model.bbox.min_x = reader.Read<double>();
	model.bbox.min_y = reader.Read<double>();
	model.bbox.min_z = reader.Read<double>();
	model.bbox.max_x = reader.Read<double>();
	model.bbox.max_y = reader.Read<double>();
	model.bbox.max_z = reader.Read<double>();

	// Offset arrays
	model.solid_shell_offsets.resize(solid_count + 1);
	reader.ReadArray(model.solid_shell_offsets.data(), solid_count + 1);

	model.shell_face_offsets.resize(shell_count + 1);
	reader.ReadArray(model.shell_face_offsets.data(), shell_count + 1);

	model.face_ring_offsets.resize(face_count + 1);
	reader.ReadArray(model.face_ring_offsets.data(), face_count + 1);

	model.ring_vertex_offsets.resize(ring_count + 1);
	reader.ReadArray(model.ring_vertex_offsets.data(), ring_count + 1);

	model.face_triangle_offsets.resize(face_count + 1);
	reader.ReadArray(model.face_triangle_offsets.data(), face_count + 1);

	// Data arrays: vertices
	model.vertices.resize(vertex_count);
	for (uint32_t i = 0; i < vertex_count; i++) {
		model.vertices[i].x = reader.Read<double>();
		model.vertices[i].y = reader.Read<double>();
		model.vertices[i].z = reader.Read<double>();
	}

	// Ring vertex indices: compute total count from ring_vertex_offsets
	uint32_t total_ring_indices = ring_count > 0 ? model.ring_vertex_offsets[ring_count] : 0;
	model.ring_vertex_indices.resize(total_ring_indices);
	reader.ReadArray(model.ring_vertex_indices.data(), total_ring_indices);

	// Triangle vertex indices
	model.triangle_vertex_indices.resize(triangle_count * 3);
	reader.ReadArray(model.triangle_vertex_indices.data(), triangle_count * 3);

	// Validation cache
	model.validation.open_edge_count = reader.Read<uint32_t>();
	model.validation.non_manifold_edge_count = reader.Read<uint32_t>();
	model.validation.degenerate_face_count = reader.Read<uint32_t>();
	model.validation.orientation_error_count = reader.Read<uint32_t>();

	uint32_t summary_flags = reader.Read<uint32_t>();
	model.validation.is_closed = (summary_flags & 0x01) != 0;
	model.validation.is_manifold = (summary_flags & 0x02) != 0;
	model.validation.is_oriented = (summary_flags & 0x04) != 0;
	model.validation.is_valid = (summary_flags & 0x08) != 0;

	ValidatePayloadModel(model, vertex_count, solid_count, shell_count, face_count, ring_count, triangle_count);

	return model;
}

} // namespace duckdb_3d
