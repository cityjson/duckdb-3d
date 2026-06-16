#include "kernel/geom_wkb_parser.hpp"

#include <cstring>
#include <stdexcept>

namespace duckdb_3d {

namespace {

//! Minimal little-endian WKB cursor.
struct Cursor {
	const uint8_t *data;
	size_t size;
	size_t pos;

	void Require(size_t n) const {
		if (pos + n > size) {
			throw std::runtime_error("ParseGeomWKB: unexpected end of WKB");
		}
	}
	uint8_t U8() {
		Require(1);
		return data[pos++];
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

Vertex3D ReadPointZ(Cursor &cur) {
	Vertex3D v;
	v.x = cur.F64();
	v.y = cur.F64();
	v.z = cur.F64();
	return v;
}

void ReadLineStringZ(Cursor &cur, GeomModel &model) {
	uint32_t point_count = cur.U32();
	model.vertices.reserve(model.vertices.size() + point_count);
	for (uint32_t i = 0; i < point_count; i++) {
		model.vertices.push_back(ReadPointZ(cur));
	}
}

} // namespace

GeomModel ParseGeomWKB(const uint8_t *data, size_t size) {
	Cursor cur{data, size, 0};

	uint8_t byte_order = cur.U8();
	if (byte_order != 1) {
		throw std::runtime_error("ParseGeomWKB: only little-endian WKB is supported");
	}
	uint32_t wkb_type = cur.U32();

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
	case 1005: { // MultiLineString Z
		model.type = GeomType::MultiLineString;
		uint32_t line_count = cur.U32();
		model.part_offsets.reserve(line_count + 1);
		model.part_offsets.push_back(0);
		for (uint32_t i = 0; i < line_count; i++) {
			uint8_t child_byte_order = cur.U8();
			if (child_byte_order != 1) {
				throw std::runtime_error("ParseGeomWKB: only little-endian WKB is supported");
			}
			uint32_t child_type = cur.U32();
			if (child_type != 1002) {
				throw std::runtime_error("ParseGeomWKB: MultiLineString Z child is not LineString Z");
			}
			ReadLineStringZ(cur, model);
			model.part_offsets.push_back(static_cast<uint32_t>(model.vertices.size()));
		}
		break;
	}
	default:
		throw std::runtime_error("ParseGeomWKB: unsupported geometry class");
	}

	model.ComputeBBox();
	return model;
}

} // namespace duckdb_3d
