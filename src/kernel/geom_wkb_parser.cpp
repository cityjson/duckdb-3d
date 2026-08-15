#include "kernel/geom_wkb_parser.hpp"
#include "kernel/wkb_io.hpp"

#include <stdexcept>

namespace duckdb_3d {

namespace {

Vertex3D ReadPointZ(WkbCursor &cur) {
	Vertex3D v;
	v.x = cur.ReadF64();
	v.y = cur.ReadF64();
	v.z = cur.ReadF64();
	return v;
}

void ReadLineStringZ(WkbCursor &cur, GeomModel &model) {
	uint32_t point_count = cur.ReadU32();
	model.vertices.reserve(model.vertices.size() + point_count);
	for (uint32_t i = 0; i < point_count; i++) {
		model.vertices.push_back(ReadPointZ(cur));
	}
}

bool IsClosingVertex(const Vertex3D &a, const Vertex3D &b) {
	return a.x == b.x && a.y == b.y && a.z == b.z;
}

//! Read a single ring: append its vertices (closing duplicate stripped) and
//! push the trailing ring boundary onto `ring_offsets`.
void ReadRingZ(WkbCursor &cur, GeomModel &model) {
	uint32_t point_count = cur.ReadU32();
	std::vector<Vertex3D> ring;
	ring.reserve(point_count);
	for (uint32_t i = 0; i < point_count; i++) {
		ring.push_back(ReadPointZ(cur));
	}
	if (ring.size() >= 2 && IsClosingVertex(ring.front(), ring.back())) {
		ring.pop_back();
	}
	model.vertices.insert(model.vertices.end(), ring.begin(), ring.end());
	model.ring_offsets.push_back(static_cast<uint32_t>(model.vertices.size()));
}

//! Read a Polygon Z body (ring count + rings), populating `ring_offsets`.
void ReadPolygonZ(WkbCursor &cur, GeomModel &model) {
	uint32_t ring_count = cur.ReadU32();
	if (model.ring_offsets.empty()) {
		model.ring_offsets.push_back(0);
	}
	for (uint32_t i = 0; i < ring_count; i++) {
		ReadRingZ(cur, model);
	}
}

//! Read a sequence of child Polygon Z values (each with its own WKB header),
//! recording a part boundary into `part_offsets` after every polygon. Shared by
//! MultiPolygon Z and PolyhedralSurface Z.
void ReadPolygonPartsZ(WkbCursor &cur, GeomModel &model) {
	uint32_t polygon_count = cur.ReadU32();
	model.ring_offsets.push_back(0);
	model.part_offsets.reserve(polygon_count + 1);
	model.part_offsets.push_back(0);
	bool outer_swap = cur.swap_bytes;
	for (uint32_t i = 0; i < polygon_count; i++) {
		cur.ReadByteOrder(); // each child Polygon carries its own byte-order flag
		uint32_t child_type = cur.ReadU32();
		if (child_type != 1003) {
			throw std::runtime_error("ParseGeomWKB: expected Polygon Z child");
		}
		ReadPolygonZ(cur, model);
		cur.swap_bytes = outer_swap;
		// Part boundary = number of rings read so far.
		model.part_offsets.push_back(static_cast<uint32_t>(model.ring_offsets.size() - 1));
	}
}

} // namespace

GeomModel ParseGeomWKB(const uint8_t *data, size_t size) {
	WkbCursor cur(data, size, "ParseGeomWKB: unexpected end of WKB");

	cur.ReadByteOrder();
	uint32_t wkb_type = cur.ReadU32();

	GeomModel model;
	switch (wkb_type) {
	case 1001: { // Point Z
		model.type = GeomType::Point;
		model.vertices.push_back(ReadPointZ(cur));
		break;
	}
	case 1002: { // LineString Z
		model.type = GeomType::LineString;
		ReadLineStringZ(cur, model);
		break;
	}
	case 1003: { // Polygon Z
		model.type = GeomType::Polygon;
		ReadPolygonZ(cur, model);
		break;
	}
	case 1004: { // MultiPoint Z
		model.type = GeomType::MultiPoint;
		uint32_t point_count = cur.ReadU32();
		model.part_offsets.reserve(point_count + 1);
		model.part_offsets.push_back(0);
		bool outer_swap = cur.swap_bytes;
		for (uint32_t i = 0; i < point_count; i++) {
			cur.ReadByteOrder();
			uint32_t child_type = cur.ReadU32();
			if (child_type != 1001) {
				throw std::runtime_error("ParseGeomWKB: MultiPoint Z child is not Point Z");
			}
			model.vertices.push_back(ReadPointZ(cur));
			cur.swap_bytes = outer_swap;
			model.part_offsets.push_back(static_cast<uint32_t>(model.vertices.size()));
		}
		break;
	}
	case 1005: { // MultiLineString Z
		model.type = GeomType::MultiLineString;
		uint32_t line_count = cur.ReadU32();
		model.part_offsets.reserve(line_count + 1);
		model.part_offsets.push_back(0);
		bool outer_swap = cur.swap_bytes;
		for (uint32_t i = 0; i < line_count; i++) {
			cur.ReadByteOrder();
			uint32_t child_type = cur.ReadU32();
			if (child_type != 1002) {
				throw std::runtime_error("ParseGeomWKB: MultiLineString Z child is not LineString Z");
			}
			ReadLineStringZ(cur, model);
			cur.swap_bytes = outer_swap;
			model.part_offsets.push_back(static_cast<uint32_t>(model.vertices.size()));
		}
		break;
	}
	case 1006: { // MultiPolygon Z
		model.type = GeomType::MultiPolygon;
		ReadPolygonPartsZ(cur, model);
		break;
	}
	case 1015: { // PolyhedralSurface Z
		model.type = GeomType::PolyhedralSurface;
		ReadPolygonPartsZ(cur, model);
		break;
	}
	default:
		throw std::runtime_error("ParseGeomWKB: unsupported geometry class");
	}

	model.ComputeBBox();
	return model;
}

} // namespace duckdb_3d
