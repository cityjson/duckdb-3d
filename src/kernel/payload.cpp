#include "kernel/payload.hpp"
#include "kernel/byte_io.hpp"
#include <cstring>
#include <stdexcept>

namespace duckdb_3d {

namespace {

constexpr const char *kTruncationMessage = "SOLID_3D payload truncated";

void ValidatePayloadModel(const SolidModel &model, uint32_t vertex_count, uint32_t solid_count, uint32_t shell_count,
                          uint32_t face_count, uint32_t ring_count, uint32_t triangle_count) {
	ValidateOffsets(model.solid_shell_offsets, shell_count, "solid-shell", "SOLID_3D payload");
	ValidateOffsets(model.shell_face_offsets, face_count, "shell-face", "SOLID_3D payload");
	ValidateOffsets(model.face_ring_offsets, ring_count, "face-ring", "SOLID_3D payload");
	ValidateOffsets(model.face_triangle_offsets, triangle_count, "face-triangle", "SOLID_3D payload");

	if (model.ring_vertex_offsets.empty() || model.ring_vertex_offsets.front() != 0) {
		throw std::runtime_error("SOLID_3D payload: invalid ring-vertex offsets");
	}
	for (size_t i = 1; i < model.ring_vertex_offsets.size(); i++) {
		if (model.ring_vertex_offsets[i] < model.ring_vertex_offsets[i - 1]) {
			throw std::runtime_error("SOLID_3D payload: non-monotonic ring-vertex offsets");
		}
	}

	if (model.vertices.size() != vertex_count || model.SolidCount() != solid_count ||
	    model.ShellCount() != shell_count || model.FaceCount() != face_count || model.RingCount() != ring_count ||
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
	ByteWriter writer;

	// Header
	writer.WriteBytes(PAYLOAD_MAGIC, 4);
	writer.WriteU16(PAYLOAD_VERSION_MAJOR);
	writer.WriteU16(PAYLOAD_VERSION_MINOR);
	writer.WriteU32(0); // flags

	uint32_t vertex_count = static_cast<uint32_t>(model.vertices.size());
	uint32_t solid_count = model.SolidCount();
	uint32_t shell_count = model.ShellCount();
	uint32_t face_count = model.FaceCount();
	uint32_t ring_count = model.RingCount();
	uint32_t triangle_count = model.TriangleCount();

	writer.WriteU32(vertex_count);
	writer.WriteU32(solid_count);
	writer.WriteU32(shell_count);
	writer.WriteU32(face_count);
	writer.WriteU32(ring_count);
	writer.WriteU32(triangle_count);

	// BBox
	writer.WriteF64(model.bbox.min_x);
	writer.WriteF64(model.bbox.min_y);
	writer.WriteF64(model.bbox.min_z);
	writer.WriteF64(model.bbox.max_x);
	writer.WriteF64(model.bbox.max_y);
	writer.WriteF64(model.bbox.max_z);

	// Offset arrays
	writer.WriteU32Array(model.solid_shell_offsets.data(), solid_count + 1);
	writer.WriteU32Array(model.shell_face_offsets.data(), shell_count + 1);
	writer.WriteU32Array(model.face_ring_offsets.data(), face_count + 1);
	writer.WriteU32Array(model.ring_vertex_offsets.data(), ring_count + 1);
	writer.WriteU32Array(model.face_triangle_offsets.data(), face_count + 1);

	// Data arrays: vertices
	for (uint32_t i = 0; i < vertex_count; i++) {
		writer.WriteF64(model.vertices[i].x);
		writer.WriteF64(model.vertices[i].y);
		writer.WriteF64(model.vertices[i].z);
	}

	// Data arrays: ring vertex indices
	writer.WriteU32Array(model.ring_vertex_indices.data(), model.ring_vertex_indices.size());

	// Data arrays: triangle vertex indices
	writer.WriteU32Array(model.triangle_vertex_indices.data(), static_cast<size_t>(triangle_count) * 3);

	// Validation cache
	writer.WriteU32(model.validation.open_edge_count);
	writer.WriteU32(model.validation.non_manifold_edge_count);
	writer.WriteU32(model.validation.degenerate_face_count);
	writer.WriteU32(model.validation.orientation_error_count);

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
	writer.WriteU32(summary_flags);

	return writer.buffer;
}

SolidPayloadInfo ReadSolidPayloadHeader(const uint8_t *data, size_t size) {
	ByteReader reader(data, size, kTruncationMessage);

	uint8_t magic[4];
	reader.ReadBytes(magic, 4);
	if (std::memcmp(magic, PAYLOAD_MAGIC, 4) != 0) {
		throw std::runtime_error("SOLID_3D payload: invalid magic bytes");
	}
	uint16_t major = reader.ReadU16();
	reader.ReadU16(); // minor
	if (major != PAYLOAD_VERSION_MAJOR) {
		throw std::runtime_error("SOLID_3D payload: unsupported major version " + std::to_string(major));
	}
	reader.ReadU32(); // flags

	SolidPayloadInfo info;
	info.vertex_count = reader.ReadU32();
	info.solid_count = reader.ReadU32();
	info.shell_count = reader.ReadU32();
	info.face_count = reader.ReadU32();
	info.ring_count = reader.ReadU32();
	info.triangle_count = reader.ReadU32();

	info.bbox.min_x = reader.ReadF64();
	info.bbox.min_y = reader.ReadF64();
	info.bbox.min_z = reader.ReadF64();
	info.bbox.max_x = reader.ReadF64();
	info.bbox.max_y = reader.ReadF64();
	info.bbox.max_z = reader.ReadF64();

	// The validation summary is the fixed trailing block: four uint32 counts
	// followed by a uint32 flag word (20 bytes total). Read it from the tail so
	// we never touch the variable-length body in between.
	constexpr size_t kValidationBlockSize = 5 * sizeof(uint32_t);
	if (size < reader.pos + kValidationBlockSize) {
		throw std::runtime_error(kTruncationMessage);
	}
	ByteReader tail(data, size, kTruncationMessage);
	tail.pos = size - kValidationBlockSize;
	info.validation.open_edge_count = tail.ReadU32();
	info.validation.non_manifold_edge_count = tail.ReadU32();
	info.validation.degenerate_face_count = tail.ReadU32();
	info.validation.orientation_error_count = tail.ReadU32();
	uint32_t summary_flags = tail.ReadU32();
	info.validation.is_closed = (summary_flags & 0x01) != 0;
	info.validation.is_manifold = (summary_flags & 0x02) != 0;
	info.validation.is_oriented = (summary_flags & 0x04) != 0;
	info.validation.is_valid = (summary_flags & 0x08) != 0;

	return info;
}

SolidModel DeserializePayload(const uint8_t *data, size_t size) {
	ByteReader reader(data, size, kTruncationMessage);

	// Verify magic
	uint8_t magic[4];
	reader.ReadBytes(magic, 4);
	if (std::memcmp(magic, PAYLOAD_MAGIC, 4) != 0) {
		throw std::runtime_error("SOLID_3D payload: invalid magic bytes");
	}

	// Verify version
	uint16_t major = reader.ReadU16();
	reader.ReadU16(); // minor
	if (major != PAYLOAD_VERSION_MAJOR) {
		throw std::runtime_error("SOLID_3D payload: unsupported major version " + std::to_string(major));
	}
	// Minor version: accept if >= our minor (forward compat for minor additions)

	uint32_t flags = reader.ReadU32();
	(void)flags; // reserved

	uint32_t vertex_count = reader.ReadU32();
	uint32_t solid_count = reader.ReadU32();
	uint32_t shell_count = reader.ReadU32();
	uint32_t face_count = reader.ReadU32();
	uint32_t ring_count = reader.ReadU32();
	uint32_t triangle_count = reader.ReadU32();

	SolidModel model;

	// BBox
	model.bbox.min_x = reader.ReadF64();
	model.bbox.min_y = reader.ReadF64();
	model.bbox.min_z = reader.ReadF64();
	model.bbox.max_x = reader.ReadF64();
	model.bbox.max_y = reader.ReadF64();
	model.bbox.max_z = reader.ReadF64();

	// Offset arrays. Bound every declared count against the bytes that remain
	// before allocating, so a malformed header cannot drive a huge allocation.
	// Counts are +1 because an N-element collection stores N+1 boundary offsets.
	reader.RequireCount(static_cast<uint64_t>(solid_count) + 1, sizeof(uint32_t), "solid-shell offset");
	model.solid_shell_offsets.resize(solid_count + 1);
	reader.ReadU32Array(model.solid_shell_offsets.data(), solid_count + 1);

	reader.RequireCount(static_cast<uint64_t>(shell_count) + 1, sizeof(uint32_t), "shell-face offset");
	model.shell_face_offsets.resize(shell_count + 1);
	reader.ReadU32Array(model.shell_face_offsets.data(), shell_count + 1);

	reader.RequireCount(static_cast<uint64_t>(face_count) + 1, sizeof(uint32_t), "face-ring offset");
	model.face_ring_offsets.resize(face_count + 1);
	reader.ReadU32Array(model.face_ring_offsets.data(), face_count + 1);

	reader.RequireCount(static_cast<uint64_t>(ring_count) + 1, sizeof(uint32_t), "ring-vertex offset");
	model.ring_vertex_offsets.resize(ring_count + 1);
	reader.ReadU32Array(model.ring_vertex_offsets.data(), ring_count + 1);

	reader.RequireCount(static_cast<uint64_t>(face_count) + 1, sizeof(uint32_t), "face-triangle offset");
	model.face_triangle_offsets.resize(face_count + 1);
	reader.ReadU32Array(model.face_triangle_offsets.data(), face_count + 1);

	// Data arrays: vertices (3 doubles each)
	reader.RequireCount(vertex_count, 3 * sizeof(double), "vertex");
	model.vertices.resize(vertex_count);
	for (uint32_t i = 0; i < vertex_count; i++) {
		model.vertices[i].x = reader.ReadF64();
		model.vertices[i].y = reader.ReadF64();
		model.vertices[i].z = reader.ReadF64();
	}

	// Ring vertex indices: compute total count from ring_vertex_offsets
	uint32_t total_ring_indices = ring_count > 0 ? model.ring_vertex_offsets[ring_count] : 0;
	reader.RequireCount(total_ring_indices, sizeof(uint32_t), "ring-vertex index");
	model.ring_vertex_indices.resize(total_ring_indices);
	reader.ReadU32Array(model.ring_vertex_indices.data(), total_ring_indices);

	// Triangle vertex indices (3 per triangle). Widen before multiplying so the
	// index count cannot overflow uint32_t for a hostile triangle_count.
	uint64_t triangle_index_count = static_cast<uint64_t>(triangle_count) * 3;
	reader.RequireCount(triangle_index_count, sizeof(uint32_t), "triangle-vertex index");
	model.triangle_vertex_indices.resize(triangle_index_count);
	reader.ReadU32Array(model.triangle_vertex_indices.data(), triangle_index_count);

	// Validation cache
	model.validation.open_edge_count = reader.ReadU32();
	model.validation.non_manifold_edge_count = reader.ReadU32();
	model.validation.degenerate_face_count = reader.ReadU32();
	model.validation.orientation_error_count = reader.ReadU32();

	uint32_t summary_flags = reader.ReadU32();
	model.validation.is_closed = (summary_flags & 0x01) != 0;
	model.validation.is_manifold = (summary_flags & 0x02) != 0;
	model.validation.is_oriented = (summary_flags & 0x04) != 0;
	model.validation.is_valid = (summary_flags & 0x08) != 0;

	ValidatePayloadModel(model, vertex_count, solid_count, shell_count, face_count, ring_count, triangle_count);

	return model;
}

} // namespace duckdb_3d
