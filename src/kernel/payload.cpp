#include "kernel/payload.hpp"
#include "kernel/byte_io.hpp"
#include <cstring>
#include <stdexcept>

namespace duckdb_3d {

namespace {

constexpr const char *kTruncationMessage = "SOLID_3D payload truncated";

//! The fixed trailing validation block: seven uint32 counts and a uint32 flag word.
constexpr size_t kValidationBlockSize = 8 * sizeof(uint32_t);

//! Out of line, so the preamble check stays small enough to inline into the
//! header read, which runs once a row.
[[noreturn]] void ThrowUnsupportedMajor(uint16_t major) {
	throw std::runtime_error("SOLID_3D payload: unsupported major version " + std::to_string(major) +
	                         " (this build reads version " + std::to_string(PAYLOAD_VERSION_MAJOR) +
	                         "); rebuild the value from its WKB with ST_3DFromWKB");
}

//! The magic and the version: the bytes before the flags.
constexpr size_t kPreambleSize = 4 + 2 * sizeof(uint16_t);

//! Check the magic and the major version at the start of `data`; the caller's
//! reader resumes at kPreambleSize. Like ReadValidation it reads with a reader
//! of its own: handing the caller's to a call that is not inlined would keep
//! its position in memory for every read that follows.
void ReadPreamble(const uint8_t *data, size_t size) {
	ByteReader reader(data, size, kTruncationMessage);
	uint8_t magic[4];
	reader.ReadBytes(magic, 4);
	if (std::memcmp(magic, PAYLOAD_MAGIC, 4) != 0) {
		throw std::runtime_error("SOLID_3D payload: invalid magic bytes");
	}
	uint16_t major = reader.ReadU16();
	reader.ReadU16(); // minor: a minor bump only appends what older readers may ignore
	if (major != PAYLOAD_VERSION_MAJOR) {
		ThrowUnsupportedMajor(major);
	}
}

void WriteValidation(ByteWriter &writer, const ValidationCache &vc) {
	writer.WriteU32(vc.open_edge_count);
	writer.WriteU32(vc.non_manifold_edge_count);
	writer.WriteU32(vc.degenerate_face_count);
	writer.WriteU32(vc.orientation_error_count);
	writer.WriteU32(vc.non_manifold_vertex_count);
	writer.WriteU32(vc.disconnected_shell_count);
	writer.WriteU32(vc.self_intersection_count);
	uint32_t summary_flags = 0;
	if (vc.is_closed) {
		summary_flags |= 0x01;
	}
	if (vc.is_manifold) {
		summary_flags |= 0x02;
	}
	if (vc.is_oriented) {
		summary_flags |= 0x04;
	}
	if (vc.is_valid) {
		summary_flags |= 0x08;
	}
	writer.WriteU32(summary_flags);
}

//! Decodes the trailing validation block at `block`, which holds
//! kValidationBlockSize bytes. A reader of its own, not the caller's: the header
//! read runs once a row, and advancing a reader through a reference stores and
//! reloads its position for every field.
ValidationCache ReadValidation(const uint8_t *block) {
	ByteReader reader(block, kValidationBlockSize, kTruncationMessage);
	ValidationCache vc;
	vc.open_edge_count = reader.ReadU32();
	vc.non_manifold_edge_count = reader.ReadU32();
	vc.degenerate_face_count = reader.ReadU32();
	vc.orientation_error_count = reader.ReadU32();
	vc.non_manifold_vertex_count = reader.ReadU32();
	vc.disconnected_shell_count = reader.ReadU32();
	vc.self_intersection_count = reader.ReadU32();
	uint32_t summary_flags = reader.ReadU32();
	vc.is_closed = (summary_flags & 0x01) != 0;
	vc.is_manifold = (summary_flags & 0x02) != 0;
	vc.is_oriented = (summary_flags & 0x04) != 0;
	vc.is_valid = (summary_flags & 0x08) != 0;
	return vc;
}

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

size_t SerializedPayloadSize(const SolidModel &model) {
	constexpr size_t kHeader = 4 + 2 + 2 + 4 + 6 * sizeof(uint32_t) + 6 * sizeof(double);
	const size_t crs = sizeof(uint32_t) + model.crs.size();
	constexpr size_t kTrailer = kValidationBlockSize;
	size_t offsets = (static_cast<size_t>(model.SolidCount()) + 1) + (static_cast<size_t>(model.ShellCount()) + 1) +
	                 2 * (static_cast<size_t>(model.FaceCount()) + 1) + (static_cast<size_t>(model.RingCount()) + 1);
	return kHeader + crs + offsets * sizeof(uint32_t) + model.vertices.size() * 3 * sizeof(double) +
	       model.ring_vertex_indices.size() * sizeof(uint32_t) +
	       static_cast<size_t>(model.TriangleCount()) * 3 * sizeof(uint32_t) + kTrailer;
}

std::vector<uint8_t> SerializePayload(const SolidModel &model) {
	ByteWriter writer;
	writer.Reserve(SerializedPayloadSize(model));

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

	// CRS: after the fixed fields, so header readers keep fixed offsets
	writer.WriteString(model.crs);

	// Offset arrays
	writer.WriteU32Array(model.solid_shell_offsets.data(), solid_count + 1);
	writer.WriteU32Array(model.shell_face_offsets.data(), shell_count + 1);
	writer.WriteU32Array(model.face_ring_offsets.data(), face_count + 1);
	writer.WriteU32Array(model.ring_vertex_offsets.data(), ring_count + 1);
	writer.WriteU32Array(model.face_triangle_offsets.data(), face_count + 1);

	// Data arrays: vertices
	writer.WriteVertices(model.vertices.data(), vertex_count);

	// Data arrays: ring vertex indices
	writer.WriteU32Array(model.ring_vertex_indices.data(), model.ring_vertex_indices.size());

	// Data arrays: triangle vertex indices
	writer.WriteU32Array(model.triangle_vertex_indices.data(), static_cast<size_t>(triangle_count) * 3);

	// Validation cache
	WriteValidation(writer, model.validation);

	return writer.buffer;
}

SolidPayloadInfo ReadSolidPayloadHeader(const uint8_t *data, size_t size) {
	ByteReader reader(data, size, kTruncationMessage);
	ReadPreamble(data, size);
	reader.pos = kPreambleSize;
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
	info.crs = reader.ReadStringView("CRS byte");

	// The validation summary is the fixed trailing block. Read it from the tail
	// so we never touch the variable-length body in between.
	if (size < reader.pos + kValidationBlockSize) {
		throw std::runtime_error(kTruncationMessage);
	}
	info.validation = ReadValidation(data + size - kValidationBlockSize);
	return info;
}

SolidModel DeserializePayload(const uint8_t *data, size_t size) {
	SolidModel model;
	DeserializePayloadInto(data, size, model);
	return model;
}

void DeserializePayloadInto(const uint8_t *data, size_t size, SolidModel &model) {
	ByteReader reader(data, size, kTruncationMessage);
	ReadPreamble(data, size);
	reader.pos = kPreambleSize;

	uint32_t flags = reader.ReadU32();
	(void)flags; // reserved

	uint32_t vertex_count = reader.ReadU32();
	uint32_t solid_count = reader.ReadU32();
	uint32_t shell_count = reader.ReadU32();
	uint32_t face_count = reader.ReadU32();
	uint32_t ring_count = reader.ReadU32();
	uint32_t triangle_count = reader.ReadU32();

	// BBox
	model.bbox.min_x = reader.ReadF64();
	model.bbox.min_y = reader.ReadF64();
	model.bbox.min_z = reader.ReadF64();
	model.bbox.max_x = reader.ReadF64();
	model.bbox.max_y = reader.ReadF64();
	model.bbox.max_z = reader.ReadF64();
	model.crs = reader.ReadString("CRS byte");

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
	reader.ReadVertices(model.vertices.data(), vertex_count);

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
	reader.Require(kValidationBlockSize);
	model.validation = ReadValidation(reader.data + reader.pos);
	reader.pos += kValidationBlockSize;

	ValidatePayloadModel(model, vertex_count, solid_count, shell_count, face_count, ring_count, triangle_count);
}

std::vector<uint8_t> WithSolidPayloadCrs(const uint8_t *data, size_t size, const std::string &crs) {
	// Magic, version, flags, six counts and the bounding box come first.
	constexpr size_t kCrsOffset = 4 + 2 + 2 + 4 + 6 * sizeof(uint32_t) + 6 * sizeof(double);
	auto info = ReadSolidPayloadHeader(data, size);
	return ReplaceCrsSlot(data, size, kCrsOffset, info.crs.size(), crs);
}

} // namespace duckdb_3d
