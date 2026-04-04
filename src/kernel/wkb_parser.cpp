#include "kernel/wkb_parser.hpp"
#include <cstring>
#include <stdexcept>

namespace duckdb_3d {

namespace {

class WKBReader {
public:
	const uint8_t *data;
	size_t size;
	size_t pos = 0;
	bool swap_bytes = false; // true if WKB is big-endian on little-endian host

	WKBReader(const uint8_t *data, size_t size) : data(data), size(size) {
	}

	void Ensure(size_t n) const {
		if (pos + n > size) {
			throw std::runtime_error("WKB data truncated");
		}
	}

	uint8_t ReadByte() {
		Ensure(1);
		return data[pos++];
	}

	uint32_t ReadU32() {
		Ensure(4);
		uint32_t v;
		std::memcpy(&v, data + pos, 4);
		pos += 4;
		if (swap_bytes) {
			v = ((v >> 24) & 0xFF) | ((v >> 8) & 0xFF00) | ((v << 8) & 0xFF0000) | ((v << 24) & 0xFF000000);
		}
		return v;
	}

	double ReadF64() {
		Ensure(8);
		double v;
		if (swap_bytes) {
			uint8_t tmp[8];
			for (int i = 0; i < 8; i++) {
				tmp[i] = data[pos + 7 - i];
			}
			std::memcpy(&v, tmp, 8);
		} else {
			std::memcpy(&v, data + pos, 8);
		}
		pos += 8;
		return v;
	}

	void ReadByteOrder() {
		uint8_t bo = ReadByte();
		// 1 = little-endian (match host assumption), 0 = big-endian
		swap_bytes = (bo == 0);
	}

	WKBGeometryType ReadGeometryType() {
		return static_cast<WKBGeometryType>(ReadU32());
	}
};

//! Check if a closing vertex duplicates the first vertex of a ring
bool IsClosingVertex(const Vertex3D &first, const Vertex3D &last) {
	return first.x == last.x && first.y == last.y && first.z == last.z;
}

ParsedPolyhedralSurface ParsePolyhedralSurface(WKBReader &reader) {
	ParsedPolyhedralSurface result;

	uint32_t num_polygons = reader.ReadU32();
	result.polygon_count = num_polygons;
	result.polygon_ring_counts.resize(num_polygons);
	result.ring_vertex_counts.reserve(num_polygons); // at least one ring per polygon

	for (uint32_t p = 0; p < num_polygons; p++) {
		uint32_t num_rings = reader.ReadU32();
		result.polygon_ring_counts[p] = num_rings;

		for (uint32_t r = 0; r < num_rings; r++) {
			uint32_t num_points = reader.ReadU32();
			std::vector<Vertex3D> ring_pts;
			ring_pts.reserve(num_points);

			for (uint32_t i = 0; i < num_points; i++) {
				Vertex3D v;
				v.x = reader.ReadF64();
				v.y = reader.ReadF64();
				v.z = reader.ReadF64();
				ring_pts.push_back(v);
			}

			// Remove WKB closing vertex if it duplicates the first
			if (ring_pts.size() >= 2 && IsClosingVertex(ring_pts.front(), ring_pts.back())) {
				ring_pts.pop_back();
			}

			result.ring_vertex_counts.push_back(static_cast<uint32_t>(ring_pts.size()));
			result.vertices.insert(result.vertices.end(), ring_pts.begin(), ring_pts.end());
		}
	}

	return result;
}

} // anonymous namespace

std::vector<ParsedPolyhedralSurface> ParseWKB(const uint8_t *data, size_t size) {
	if (data == nullptr || size == 0) {
		throw std::runtime_error("WKB data is empty");
	}

	WKBReader reader(data, size);
	reader.ReadByteOrder();
	auto geom_type = reader.ReadGeometryType();

	std::vector<ParsedPolyhedralSurface> results;

	if (geom_type == WKBGeometryType::PolyhedralSurfaceZ) {
		results.push_back(ParsePolyhedralSurface(reader));
	} else if (geom_type == WKBGeometryType::GeometryCollectionZ) {
		uint32_t num_children = reader.ReadU32();
		for (uint32_t i = 0; i < num_children; i++) {
			reader.ReadByteOrder();
			auto child_type = reader.ReadGeometryType();
			if (child_type != WKBGeometryType::PolyhedralSurfaceZ) {
				throw std::runtime_error(
				    "Unsupported geometry type in GeometryCollection Z: expected PolyhedralSurface Z, got type code " +
				    std::to_string(static_cast<uint32_t>(child_type)));
			}
			results.push_back(ParsePolyhedralSurface(reader));
		}
	} else {
		throw std::runtime_error("Unsupported WKB geometry type for SOLID_3D import: type code " +
		                         std::to_string(static_cast<uint32_t>(geom_type)));
	}

	return results;
}

} // namespace duckdb_3d
