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
		Vertex3D v;
		v.x = cur.F64();
		v.y = cur.F64();
		v.z = cur.F64();
		model.vertices.push_back(v);
		break;
	}
	case 1002: { // LineString Z
		model.type = GeomType::LineString;
		uint32_t point_count = cur.U32();
		model.vertices.reserve(point_count);
		for (uint32_t i = 0; i < point_count; i++) {
			Vertex3D v;
			v.x = cur.F64();
			v.y = cur.F64();
			v.z = cur.F64();
			model.vertices.push_back(v);
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
