#include "kernel/geom_payload.hpp"
#include "kernel/byte_io.hpp"
#include "kernel/triangulation.hpp"

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

	// The triangulation cache: one range per polygon, indices into the vertices.
	uint32_t polygon_count = model.PolygonCount();
	if (polygon_count == 0) {
		if (!model.polygon_triangle_offsets.empty() || !model.triangle_indices.empty()) {
			throw std::runtime_error("GEOM_3D payload: triangles on a geometry without polygons");
		}
		return;
	}
	if (model.polygon_triangle_offsets.size() != polygon_count + 1) {
		throw std::runtime_error("GEOM_3D payload: triangle offsets do not match the polygon count");
	}
	ValidateOffsets(model.polygon_triangle_offsets, static_cast<uint32_t>(model.triangle_indices.size() / 3),
	                "polygon-triangle", "GEOM_3D payload");
	if (model.triangle_indices.size() % 3 != 0) {
		throw std::runtime_error("GEOM_3D payload: triangle index count is not a multiple of three");
	}
	for (auto i : model.triangle_indices) {
		if (i >= vertex_count) {
			throw std::runtime_error("GEOM_3D payload: triangle vertex index out of range");
		}
	}
}

//! Check the magic and the major version, leaving `r` after the version.
void ReadPreamble(ByteReader &r, const char *who) {
	r.Require(4);
	if (std::memcmp(r.data, GEOM_PAYLOAD_MAGIC, 4) != 0) {
		throw std::runtime_error(std::string(who) + ": bad magic (not a GEOM_3D value)");
	}
	r.pos = 4;
	uint16_t major = r.ReadU16();
	r.ReadU16(); // minor
	if (major != GEOM_PAYLOAD_VERSION_MAJOR) {
		throw std::runtime_error(std::string(who) + ": unsupported major version " + std::to_string(major) +
		                         " (this build reads version " + std::to_string(GEOM_PAYLOAD_VERSION_MAJOR) +
		                         "); rebuild the value from its WKB with ST_Geom3DFromWKB");
	}
}

void ReadU32Vector(ByteReader &r, std::vector<uint32_t> &out, const char *what) {
	uint32_t count = r.ReadU32();
	r.RequireCount(count, sizeof(uint32_t), what);
	out.resize(count);
	r.ReadU32Array(out.data(), count);
}

} // namespace

std::vector<uint8_t> SerializeGeomPayload(const GeomModel &model) {
	// Every GEOM_3D value carries its polygons' triangles. Fill them in here when
	// the caller has not, so no constructor can produce a value without them.
	const GeomModel *source = &model;
	GeomModel triangulated;
	if (model.PolygonCount() > 0 && model.polygon_triangle_offsets.size() != model.PolygonCount() + 1) {
		triangulated = model;
		TriangulateGeomModel(triangulated);
		source = &triangulated;
	}
	const GeomModel &m = *source;

	ByteWriter w;
	w.WriteBytes(GEOM_PAYLOAD_MAGIC, 4);
	w.WriteU16(GEOM_PAYLOAD_VERSION_MAJOR);
	w.WriteU16(GEOM_PAYLOAD_VERSION_MINOR);
	w.WriteU32(static_cast<uint32_t>(m.type));

	w.WriteF64(m.bbox.min_x);
	w.WriteF64(m.bbox.min_y);
	w.WriteF64(m.bbox.min_z);
	w.WriteF64(m.bbox.max_x);
	w.WriteF64(m.bbox.max_y);
	w.WriteF64(m.bbox.max_z);
	w.WriteU32(static_cast<uint32_t>(m.vertices.size()));
	// CRS: after the fixed fields, so header readers keep fixed offsets
	w.WriteString(m.crs);

	w.WriteVertices(m.vertices.data(), m.vertices.size());
	w.WriteU32(static_cast<uint32_t>(m.ring_offsets.size()));
	w.WriteU32Array(m.ring_offsets.data(), m.ring_offsets.size());
	w.WriteU32(static_cast<uint32_t>(m.part_offsets.size()));
	w.WriteU32Array(m.part_offsets.data(), m.part_offsets.size());
	w.WriteU32(static_cast<uint32_t>(m.polygon_triangle_offsets.size()));
	w.WriteU32Array(m.polygon_triangle_offsets.data(), m.polygon_triangle_offsets.size());
	w.WriteU32(static_cast<uint32_t>(m.triangle_indices.size()));
	w.WriteU32Array(m.triangle_indices.data(), m.triangle_indices.size());
	return w.buffer;
}

GeomPayloadInfo ReadGeomPayloadHeader(const uint8_t *data, size_t size) {
	ByteReader r(data, size, kTruncationMessage);
	ReadPreamble(r, "ReadGeomPayloadHeader");

	GeomPayloadInfo info;
	info.type = static_cast<GeomType>(r.ReadU32());
	info.bbox.min_x = r.ReadF64();
	info.bbox.min_y = r.ReadF64();
	info.bbox.min_z = r.ReadF64();
	info.bbox.max_x = r.ReadF64();
	info.bbox.max_y = r.ReadF64();
	info.bbox.max_z = r.ReadF64();
	info.vertex_count = r.ReadU32();
	info.crs = r.ReadString("CRS byte");
	return info;
}

GeomModel DeserializeGeomPayload(const uint8_t *data, size_t size) {
	ByteReader r(data, size, kTruncationMessage);
	ReadPreamble(r, "DeserializeGeomPayload");

	GeomModel model;
	model.type = static_cast<GeomType>(r.ReadU32());

	model.bbox.min_x = r.ReadF64();
	model.bbox.min_y = r.ReadF64();
	model.bbox.min_z = r.ReadF64();
	model.bbox.max_x = r.ReadF64();
	model.bbox.max_y = r.ReadF64();
	model.bbox.max_z = r.ReadF64();

	uint32_t vertex_count = r.ReadU32();
	model.crs = r.ReadString("CRS byte");
	r.RequireCount(vertex_count, 3 * sizeof(double), "vertex");
	model.vertices.resize(vertex_count);
	r.ReadVertices(model.vertices.data(), vertex_count);
	ReadU32Vector(r, model.ring_offsets, "ring-offset");
	ReadU32Vector(r, model.part_offsets, "part-offset");
	ReadU32Vector(r, model.polygon_triangle_offsets, "polygon-triangle offset");
	ReadU32Vector(r, model.triangle_indices, "triangle index");
	ValidateGeomModel(model);
	return model;
}

std::vector<uint8_t> RebuildGeomPayload(const uint8_t *data, size_t size) {
	auto model = DeserializeGeomPayload(data, size);
	model.ComputeBBox();
	model.polygon_triangle_offsets.clear();
	model.triangle_indices.clear();
	return SerializeGeomPayload(model);
}

std::vector<uint8_t> WithGeomPayloadCrs(const uint8_t *data, size_t size, const std::string &crs) {
	// Magic, version, type, the bounding box and the vertex count come first.
	constexpr size_t kCrsOffset = 4 + 2 + 2 + 4 + 6 * sizeof(double) + sizeof(uint32_t);
	auto info = ReadGeomPayloadHeader(data, size);
	return ReplaceCrsSlot(data, size, kCrsOffset, info.crs.size(), crs);
}

} // namespace duckdb_3d
