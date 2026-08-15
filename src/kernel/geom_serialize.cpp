#include "kernel/geom_serialize.hpp"
#include "kernel/wkb_io.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <stdexcept>

namespace duckdb_3d {

namespace {

// ──────────────────────────────────────────────────────────────
// Common helpers
// ──────────────────────────────────────────────────────────────

std::string FormatDouble(double v) {
	// Use %.9g for a compact but precise representation.
	char buf[64];
	std::snprintf(buf, sizeof(buf), "%.9g", v);
	return std::string(buf);
}

std::string VertexWKT(const Vertex3D &v) {
	return FormatDouble(v.x) + " " + FormatDouble(v.y) + " " + FormatDouble(v.z);
}

// ──────────────────────────────────────────────────────────────
// WKT
// ──────────────────────────────────────────────────────────────

std::string RingWKT(const GeomModel &m, uint32_t begin, uint32_t end, bool close) {
	std::string out;
	for (uint32_t i = begin; i < end; i++) {
		if (i > begin) {
			out += ", ";
		}
		out += VertexWKT(m.vertices[i]);
	}
	if (close && end > begin) {
		out += ", ";
		out += VertexWKT(m.vertices[begin]);
	}
	return out;
}

std::string Geom3DAsTextInternal(const GeomModel &m) {
	switch (m.type) {
	case GeomType::Point: {
		if (m.vertices.empty()) {
			return "POINT Z EMPTY";
		}
		return "POINT Z (" + VertexWKT(m.vertices[0]) + ")";
	}
	case GeomType::MultiPoint: {
		if (m.vertices.empty()) {
			return "MULTIPOINT Z EMPTY";
		}
		std::string out = "MULTIPOINT Z (";
		for (size_t i = 0; i < m.vertices.size(); i++) {
			if (i > 0) {
				out += ", ";
			}
			out += "(" + VertexWKT(m.vertices[i]) + ")";
		}
		out += ")";
		return out;
	}
	case GeomType::LineString: {
		if (m.vertices.empty()) {
			return "LINESTRING Z EMPTY";
		}
		return "LINESTRING Z (" + RingWKT(m, 0, static_cast<uint32_t>(m.vertices.size()), false) + ")";
	}
	case GeomType::MultiLineString: {
		if (m.part_offsets.size() < 2) {
			return "MULTILINESTRING Z EMPTY";
		}
		std::string out = "MULTILINESTRING Z (";
		for (size_t k = 0; k + 1 < m.part_offsets.size(); k++) {
			if (k > 0) {
				out += ", ";
			}
			out += "(" + RingWKT(m, m.part_offsets[k], m.part_offsets[k + 1], false) + ")";
		}
		out += ")";
		return out;
	}
	case GeomType::Polygon: {
		if (m.ring_offsets.size() < 2) {
			return "POLYGON Z EMPTY";
		}
		std::string out = "POLYGON Z (";
		for (size_t r = 0; r + 1 < m.ring_offsets.size(); r++) {
			if (r > 0) {
				out += ", ";
			}
			out += "(" + RingWKT(m, m.ring_offsets[r], m.ring_offsets[r + 1], true) + ")";
		}
		out += ")";
		return out;
	}
	case GeomType::MultiPolygon:
	case GeomType::PolyhedralSurface: {
		if (m.part_offsets.size() < 2) {
			std::string keyword = (m.type == GeomType::MultiPolygon) ? "MULTIPOLYGON Z" : "POLYHEDRALSURFACE Z";
			return keyword + " EMPTY";
		}
		std::string keyword = (m.type == GeomType::MultiPolygon) ? "MULTIPOLYGON Z" : "POLYHEDRALSURFACE Z";
		std::string out = keyword + " (";
		for (size_t k = 0; k + 1 < m.part_offsets.size(); k++) {
			if (k > 0) {
				out += ", ";
			}
			out += "(";
			for (uint32_t r = m.part_offsets[k]; r < m.part_offsets[k + 1]; r++) {
				if (r > m.part_offsets[k]) {
					out += ", ";
				}
				out += "(" + RingWKT(m, m.ring_offsets[r], m.ring_offsets[r + 1], true) + ")";
			}
			out += ")";
		}
		out += ")";
		return out;
	}
	default:
		return "GEOMETRY Z EMPTY";
	}
}

// ──────────────────────────────────────────────────────────────
// GeoJSON
// ──────────────────────────────────────────────────────────────

std::string CoordJSON(const Vertex3D &v) {
	return "[" + FormatDouble(v.x) + "," + FormatDouble(v.y) + "," + FormatDouble(v.z) + "]";
}

std::string RingCoordsJSON(const GeomModel &m, uint32_t begin, uint32_t end, bool close) {
	std::string out = "[";
	for (uint32_t i = begin; i < end; i++) {
		if (i > begin) {
			out += ",";
		}
		out += CoordJSON(m.vertices[i]);
	}
	if (close && end > begin) {
		out += "," + CoordJSON(m.vertices[begin]);
	}
	out += "]";
	return out;
}

std::string Geom3DAsGeoJSONInternal(const GeomModel &m) {
	switch (m.type) {
	case GeomType::Point: {
		if (m.vertices.empty()) {
			return R"({"type":"Point","coordinates":[]})";
		}
		return R"({"type":"Point","coordinates":)" + CoordJSON(m.vertices[0]) + "}";
	}
	case GeomType::MultiPoint: {
		std::string out = R"({"type":"MultiPoint","coordinates":[)";
		for (size_t i = 0; i < m.vertices.size(); i++) {
			if (i > 0) {
				out += ",";
			}
			out += CoordJSON(m.vertices[i]);
		}
		out += "]}";
		return out;
	}
	case GeomType::LineString: {
		return R"({"type":"LineString","coordinates":)" +
		       RingCoordsJSON(m, 0, static_cast<uint32_t>(m.vertices.size()), false) + "}";
	}
	case GeomType::MultiLineString: {
		std::string out = R"({"type":"MultiLineString","coordinates":[)";
		for (size_t k = 0; k + 1 < m.part_offsets.size(); k++) {
			if (k > 0) {
				out += ",";
			}
			out += RingCoordsJSON(m, m.part_offsets[k], m.part_offsets[k + 1], false);
		}
		out += "]}";
		return out;
	}
	case GeomType::Polygon: {
		std::string out = R"({"type":"Polygon","coordinates":[)";
		for (size_t r = 0; r + 1 < m.ring_offsets.size(); r++) {
			if (r > 0) {
				out += ",";
			}
			out += RingCoordsJSON(m, m.ring_offsets[r], m.ring_offsets[r + 1], true);
		}
		out += "]}";
		return out;
	}
	case GeomType::MultiPolygon:
	case GeomType::PolyhedralSurface: {
		std::string out = R"({"type":"MultiPolygon","coordinates":[)";
		for (size_t k = 0; k + 1 < m.part_offsets.size(); k++) {
			if (k > 0) {
				out += ",";
			}
			out += "[";
			for (uint32_t r = m.part_offsets[k]; r < m.part_offsets[k + 1]; r++) {
				if (r > m.part_offsets[k]) {
					out += ",";
				}
				out += RingCoordsJSON(m, m.ring_offsets[r], m.ring_offsets[r + 1], true);
			}
			out += "]";
		}
		out += "]}";
		return out;
	}
	default:
		return R"({"type":"GeometryCollection","geometries":[]})";
	}
}

// ──────────────────────────────────────────────────────────────
// WKB
// ──────────────────────────────────────────────────────────────

enum class WKBType : uint32_t {
	PointZ = 1001,
	LineStringZ = 1002,
	PolygonZ = 1003,
	MultiPointZ = 1004,
	MultiLineStringZ = 1005,
	MultiPolygonZ = 1006,
	PolyhedralSurfaceZ = 1015,
	GeometryCollectionZ = 1007
};

//! The shared little-endian writer plus this serialiser's own type-code and
//! vertex writes.
class WKBWriter : public WkbLEWriter {
public:
	void WriteType(WKBType type) {
		WriteU32(static_cast<uint32_t>(type));
	}
	void WritePoint(const Vertex3D &v) {
		WriteF64(v.x);
		WriteF64(v.y);
		WriteF64(v.z);
	}
};

void WriteRingWKB(WKBWriter &w, const GeomModel &m, uint32_t begin, uint32_t end) {
	uint32_t n = end - begin;
	w.WriteU32(n + 1);
	for (uint32_t i = begin; i < end; i++) {
		w.WritePoint(m.vertices[i]);
	}
	w.WritePoint(m.vertices[begin]);
}

void WritePolygonWKB(WKBWriter &w, const GeomModel &m, uint32_t part_idx) {
	w.WriteByteOrder();
	w.WriteType(WKBType::PolygonZ);
	uint32_t ring_begin = m.part_offsets[part_idx];
	uint32_t ring_end = m.part_offsets[part_idx + 1];
	w.WriteU32(ring_end - ring_begin);
	for (uint32_t r = ring_begin; r < ring_end; r++) {
		WriteRingWKB(w, m, m.ring_offsets[r], m.ring_offsets[r + 1]);
	}
}

void WriteGeomWKB(WKBWriter &w, const GeomModel &m) {
	switch (m.type) {
	case GeomType::Point: {
		w.WriteByteOrder();
		w.WriteType(WKBType::PointZ);
		if (!m.vertices.empty()) {
			w.WritePoint(m.vertices[0]);
		}
		break;
	}
	case GeomType::MultiPoint: {
		w.WriteByteOrder();
		w.WriteType(WKBType::MultiPointZ);
		w.WriteU32(static_cast<uint32_t>(m.vertices.size()));
		for (const auto &v : m.vertices) {
			w.WriteByteOrder();
			w.WriteType(WKBType::PointZ);
			w.WritePoint(v);
		}
		break;
	}
	case GeomType::LineString: {
		w.WriteByteOrder();
		w.WriteType(WKBType::LineStringZ);
		w.WriteU32(static_cast<uint32_t>(m.vertices.size()));
		for (const auto &v : m.vertices) {
			w.WritePoint(v);
		}
		break;
	}
	case GeomType::MultiLineString: {
		w.WriteByteOrder();
		w.WriteType(WKBType::MultiLineStringZ);
		w.WriteU32(static_cast<uint32_t>(m.part_offsets.size() - 1));
		for (size_t k = 0; k + 1 < m.part_offsets.size(); k++) {
			w.WriteByteOrder();
			w.WriteType(WKBType::LineStringZ);
			w.WriteU32(m.part_offsets[k + 1] - m.part_offsets[k]);
			for (uint32_t i = m.part_offsets[k]; i < m.part_offsets[k + 1]; i++) {
				w.WritePoint(m.vertices[i]);
			}
		}
		break;
	}
	case GeomType::Polygon: {
		w.WriteByteOrder();
		w.WriteType(WKBType::PolygonZ);
		w.WriteU32(static_cast<uint32_t>(m.ring_offsets.size() - 1));
		for (size_t r = 0; r + 1 < m.ring_offsets.size(); r++) {
			WriteRingWKB(w, m, m.ring_offsets[r], m.ring_offsets[r + 1]);
		}
		break;
	}
	case GeomType::MultiPolygon: {
		w.WriteByteOrder();
		w.WriteType(WKBType::MultiPolygonZ);
		w.WriteU32(static_cast<uint32_t>(m.part_offsets.size() - 1));
		for (size_t k = 0; k + 1 < m.part_offsets.size(); k++) {
			WritePolygonWKB(w, m, k);
		}
		break;
	}
	case GeomType::PolyhedralSurface: {
		w.WriteByteOrder();
		w.WriteType(WKBType::PolyhedralSurfaceZ);
		w.WriteU32(static_cast<uint32_t>(m.part_offsets.size() - 1));
		for (size_t k = 0; k + 1 < m.part_offsets.size(); k++) {
			WritePolygonWKB(w, m, k);
		}
		break;
	}
	default:
		throw std::runtime_error("Geom3DAsBinary: unsupported geometry type");
	}
}

} // namespace

std::string Geom3DAsText(const GeomModel &geom) {
	return Geom3DAsTextInternal(geom);
}

std::string Geom3DAsGeoJSON(const GeomModel &geom) {
	return Geom3DAsGeoJSONInternal(geom);
}

std::vector<uint8_t> Geom3DAsBinary(const GeomModel &geom) {
	WKBWriter w;
	WriteGeomWKB(w, geom);
	return w.buffer;
}

} // namespace duckdb_3d
