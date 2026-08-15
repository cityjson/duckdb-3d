#include "kernel/wkb_parser.hpp"
#include "kernel/wkb_io.hpp"
#include <stdexcept>

namespace duckdb_3d {

namespace {

//! The shared endian-aware cursor plus this parser's own type-code read and its
//! historical truncation message (pinned by test/sql/st_3d_from_wkb.test).
class WKBReader : public WkbCursor {
public:
	WKBReader(const uint8_t *data, size_t size) : WkbCursor(data, size, "WKB data truncated") {
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

	// Per ISO WKB, each child Polygon under a PolyhedralSurface is a full
	// nested WKB value with its own byte-order byte and type code (1003).
	bool outer_swap = reader.swap_bytes;
	for (uint32_t p = 0; p < num_polygons; p++) {
		reader.ReadByteOrder();
		auto poly_type = reader.ReadGeometryType();
		if (poly_type != WKBGeometryType::PolygonZ) {
			throw std::runtime_error(
			    "Unsupported polygon type in PolyhedralSurface Z: expected Polygon Z (1003), got type code " +
			    std::to_string(static_cast<uint32_t>(poly_type)));
		}

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

		reader.swap_bytes = outer_swap;
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
