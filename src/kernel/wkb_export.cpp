#include "kernel/wkb_export.hpp"
#include "kernel/wkb_parser.hpp"
#include <cstring>

namespace duckdb_3d {

namespace {

class WKBWriter {
public:
	std::vector<uint8_t> buffer;

	void WriteByte(uint8_t v) {
		buffer.push_back(v);
	}

	void WriteU32LE(uint32_t v) {
		buffer.push_back(v & 0xFF);
		buffer.push_back((v >> 8) & 0xFF);
		buffer.push_back((v >> 16) & 0xFF);
		buffer.push_back((v >> 24) & 0xFF);
	}

	void WriteF64LE(double v) {
		uint8_t bytes[8];
		std::memcpy(bytes, &v, 8);
		buffer.insert(buffer.end(), bytes, bytes + 8);
	}

	void WriteByteOrder() {
		WriteByte(1); // little-endian
	}

	void WriteGeometryType(WKBGeometryType type) {
		WriteU32LE(static_cast<uint32_t>(type));
	}
};

//! Write a single solid (one shell) as a PolyhedralSurface Z
void WritePolyhedralSurface(WKBWriter &writer, const SolidModel &model, uint32_t solid_idx) {
	writer.WriteByteOrder();
	writer.WriteGeometryType(WKBGeometryType::PolyhedralSurfaceZ);

	uint32_t shell_start = model.solid_shell_offsets[solid_idx];
	uint32_t shell_end = model.solid_shell_offsets[solid_idx + 1];

	// Count total faces across all shells of this solid
	uint32_t total_faces = 0;
	for (uint32_t s = shell_start; s < shell_end; s++) {
		total_faces += model.shell_face_offsets[s + 1] - model.shell_face_offsets[s];
	}
	writer.WriteU32LE(total_faces);

	// Write each face as a polygon
	for (uint32_t s = shell_start; s < shell_end; s++) {
		uint32_t face_start = model.shell_face_offsets[s];
		uint32_t face_end = model.shell_face_offsets[s + 1];

		for (uint32_t f = face_start; f < face_end; f++) {
			uint32_t ring_start = model.face_ring_offsets[f];
			uint32_t ring_end = model.face_ring_offsets[f + 1];
			uint32_t num_rings = ring_end - ring_start;
			writer.WriteU32LE(num_rings);

			for (uint32_t r = ring_start; r < ring_end; r++) {
				uint32_t vi_start = model.ring_vertex_offsets[r];
				uint32_t vi_end = model.ring_vertex_offsets[r + 1];
				uint32_t num_verts = vi_end - vi_start;

				// WKB rings include closing vertex
				writer.WriteU32LE(num_verts + 1);
				for (uint32_t vi = vi_start; vi < vi_end; vi++) {
					uint32_t idx = model.ring_vertex_indices[vi];
					writer.WriteF64LE(model.vertices[idx].x);
					writer.WriteF64LE(model.vertices[idx].y);
					writer.WriteF64LE(model.vertices[idx].z);
				}
				// Closing vertex = first vertex of ring
				uint32_t first_idx = model.ring_vertex_indices[vi_start];
				writer.WriteF64LE(model.vertices[first_idx].x);
				writer.WriteF64LE(model.vertices[first_idx].y);
				writer.WriteF64LE(model.vertices[first_idx].z);
			}
		}
	}
}

} // anonymous namespace

std::vector<uint8_t> ExportWKB(const SolidModel &model) {
	uint32_t solid_count = model.SolidCount();

	if (solid_count == 1) {
		WKBWriter writer;
		WritePolyhedralSurface(writer, model, 0);
		return writer.buffer;
	}

	// Multi-solid: wrap in GeometryCollection Z
	WKBWriter writer;
	writer.WriteByteOrder();
	writer.WriteGeometryType(WKBGeometryType::GeometryCollectionZ);
	writer.WriteU32LE(solid_count);

	for (uint32_t s = 0; s < solid_count; s++) {
		WritePolyhedralSurface(writer, model, s);
	}

	return writer.buffer;
}

} // namespace duckdb_3d
