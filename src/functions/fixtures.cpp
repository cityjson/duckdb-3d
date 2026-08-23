#include "functions/three_d_functions.hpp"

#include "duckdb/function/scalar_function.hpp"

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace duckdb {

// ──────────────────────────────────────────────────────────────
// Helper: generate a test tetrahedron WKB (for SQL tests)
// ──────────────────────────────────────────────────────────────
static std::vector<uint8_t> BuildTetrahedronWKB() {
	std::vector<uint8_t> buf;
	auto u8 = [&](uint8_t v) {
		buf.push_back(v);
	};
	auto u32 = [&](uint32_t v) {
		buf.push_back(v & 0xFF);
		buf.push_back((v >> 8) & 0xFF);
		buf.push_back((v >> 16) & 0xFF);
		buf.push_back((v >> 24) & 0xFF);
	};
	auto f64 = [&](double v) {
		uint8_t b[8];
		memcpy(b, &v, 8);
		buf.insert(buf.end(), b, b + 8);
	};
	auto ring = [&](double x0, double y0, double z0, double x1, double y1, double z1, double x2, double y2, double z2) {
		u32(4);
		f64(x0);
		f64(y0);
		f64(z0);
		f64(x1);
		f64(y1);
		f64(z1);
		f64(x2);
		f64(y2);
		f64(z2);
		f64(x0);
		f64(y0);
		f64(z0);
	};
	auto poly_header = [&](uint32_t num_rings) {
		u8(1);
		u32(1003);
		u32(num_rings); // byte-order, PolygonZ, num_rings
	};

	u8(1);
	u32(1015);
	u32(4);
	poly_header(1);
	ring(0, 0, 0, 0, 1, 0, 1, 0, 0);
	poly_header(1);
	ring(0, 0, 0, 1, 0, 0, 0, 0, 1);
	poly_header(1);
	ring(1, 0, 0, 0, 1, 0, 0, 0, 1);
	poly_header(1);
	ring(0, 0, 0, 0, 0, 1, 0, 1, 0);
	return buf;
}

static void ST_AsWKBPolyhedralTetraFun(DataChunk &args, ExpressionState &state, Vector &result) {
	auto wkb = BuildTetrahedronWKB();
	auto blob_str = string_t(reinterpret_cast<const char *>(wkb.data()), wkb.size());
	result.SetVectorType(VectorType::CONSTANT_VECTOR);
	ConstantVector::GetData<string_t>(result)[0] = StringVector::AddStringOrBlob(result, blob_str);
}

// Test helper: a tetrahedron with the bottom face removed → open shell (3 faces).
static std::vector<uint8_t> BuildOpenTetrahedronWKB() {
	std::vector<uint8_t> buf;
	auto u8 = [&](uint8_t v) {
		buf.push_back(v);
	};
	auto u32 = [&](uint32_t v) {
		buf.push_back(v & 0xFF);
		buf.push_back((v >> 8) & 0xFF);
		buf.push_back((v >> 16) & 0xFF);
		buf.push_back((v >> 24) & 0xFF);
	};
	auto f64 = [&](double v) {
		uint8_t b[8];
		memcpy(b, &v, 8);
		buf.insert(buf.end(), b, b + 8);
	};
	auto ring = [&](double x0, double y0, double z0, double x1, double y1, double z1, double x2, double y2, double z2) {
		u32(4);
		f64(x0);
		f64(y0);
		f64(z0);
		f64(x1);
		f64(y1);
		f64(z1);
		f64(x2);
		f64(y2);
		f64(z2);
		f64(x0);
		f64(y0);
		f64(z0);
	};
	auto poly_header = [&](uint32_t num_rings) {
		u8(1);
		u32(1003);
		u32(num_rings); // byte-order, PolygonZ, num_rings
	};

	u8(1);
	u32(1015);
	u32(3);
	poly_header(1);
	ring(0, 0, 0, 1, 0, 0, 0, 0, 1);
	poly_header(1);
	ring(1, 0, 0, 0, 1, 0, 0, 0, 1);
	poly_header(1);
	ring(0, 0, 0, 0, 0, 1, 0, 1, 0);
	return buf;
}

static void ST_AsWKBOpenTetraFun(DataChunk &args, ExpressionState &state, Vector &result) {
	auto wkb = BuildOpenTetrahedronWKB();
	auto blob_str = string_t(reinterpret_cast<const char *>(wkb.data()), wkb.size());
	result.SetVectorType(VectorType::CONSTANT_VECTOR);
	ConstantVector::GetData<string_t>(result)[0] = StringVector::AddStringOrBlob(result, blob_str);
}

// WKB helpers shared by the cube-based test fixtures below.
namespace {
void WkbU8(std::vector<uint8_t> &buf, uint8_t v) {
	buf.push_back(v);
}
void WkbU32(std::vector<uint8_t> &buf, uint32_t v) {
	buf.push_back(v & 0xFF);
	buf.push_back((v >> 8) & 0xFF);
	buf.push_back((v >> 16) & 0xFF);
	buf.push_back((v >> 24) & 0xFF);
}
void WkbF64(std::vector<uint8_t> &buf, double v) {
	uint8_t b[8];
	memcpy(b, &v, 8);
	buf.insert(buf.end(), b, b + 8);
}
//! Emit one quad face (Polygon Z with a single closed ring of 4 corners).
void WkbQuad(std::vector<uint8_t> &buf, const double c[4][3]) {
	WkbU8(buf, 1);
	WkbU32(buf, 1003); // PolygonZ
	WkbU32(buf, 1);    // one ring
	WkbU32(buf, 5);    // 4 corners + closing vertex
	for (int i = 0; i < 4; i++) {
		WkbF64(buf, c[i][0]);
		WkbF64(buf, c[i][1]);
		WkbF64(buf, c[i][2]);
	}
	WkbF64(buf, c[0][0]);
	WkbF64(buf, c[0][1]);
	WkbF64(buf, c[0][2]);
}
//! Append the 6 quad faces of an axis-aligned cube [lo,hi]^3 to buf.
//! reversed=false → outward-facing; reversed=true → inward-facing (interior shell).
void WkbCubeFaces(std::vector<uint8_t> &buf, double lo, double hi, bool reversed) {
	// Six outward-wound faces as corner-quads.
	double faces[6][4][3] = {
	    {{lo, lo, lo}, {lo, hi, lo}, {hi, hi, lo}, {hi, lo, lo}}, // bottom z=lo
	    {{lo, lo, hi}, {hi, lo, hi}, {hi, hi, hi}, {lo, hi, hi}}, // top z=hi
	    {{lo, lo, lo}, {hi, lo, lo}, {hi, lo, hi}, {lo, lo, hi}}, // front y=lo
	    {{lo, hi, lo}, {lo, hi, hi}, {hi, hi, hi}, {hi, hi, lo}}, // back y=hi
	    {{lo, lo, lo}, {lo, lo, hi}, {lo, hi, hi}, {lo, hi, lo}}, // left x=lo
	    {{hi, lo, lo}, {hi, hi, lo}, {hi, hi, hi}, {hi, lo, hi}}, // right x=hi
	};
	for (auto &f : faces) {
		if (reversed) {
			double r[4][3];
			for (int i = 0; i < 4; i++) {
				for (int k = 0; k < 3; k++) {
					r[i][k] = f[3 - i][k];
				}
			}
			WkbQuad(buf, r);
		} else {
			WkbQuad(buf, f);
		}
	}
}
} // namespace

// Test helper: a hollow cube — outer cube [0,4]^3 (outward) enclosing inner cube
// [1,3]^3 (inward) as a single 12-face PolyhedralSurface Z. Paired with
// geometry_properties {"type":"Solid","shells":[6,6]} it imports as one
// solid with two shells: volume 64-8=56, surface area 96+24=120.
static std::vector<uint8_t> BuildHollowCubeWKB() {
	std::vector<uint8_t> buf;
	WkbU8(buf, 1);
	WkbU32(buf, 1015); // PolyhedralSurfaceZ
	WkbU32(buf, 12);   // 12 faces
	WkbCubeFaces(buf, 0.0, 4.0, /*reversed=*/false);
	WkbCubeFaces(buf, 1.0, 3.0, /*reversed=*/true);
	return buf;
}

static void ST_AsWKBHollowCubeFun(DataChunk &args, ExpressionState &state, Vector &result) {
	auto wkb = BuildHollowCubeWKB();
	auto blob_str = string_t(reinterpret_cast<const char *>(wkb.data()), wkb.size());
	result.SetVectorType(VectorType::CONSTANT_VECTOR);
	ConstantVector::GetData<string_t>(result)[0] = StringVector::AddStringOrBlob(result, blob_str);
}

// Test helper: two disjoint outward cubes ([0,2]^3 and [5,7]^3, each volume 8) as
// a GeometryCollection Z of two PolyhedralSurface Z — a MultiSolid analogue.
// Imports (plain path) as two single-shell solids: ST_3DNumSolids=2,
// total volume 16, total surface area 48.
// The one-argument overload takes the separation between the two cubes' origins
// (applied on all three axes, so the parts are offset diagonally). Total volume
// stays 16 at every separation — that is the invariant the regression pins.
static std::vector<uint8_t> BuildMultiCubeWKB(double separation = 5.0) {
	std::vector<uint8_t> buf;
	WkbU8(buf, 1);
	WkbU32(buf, 1007); // GeometryCollectionZ
	WkbU32(buf, 2);    // two members
	for (double base : {0.0, separation}) {
		WkbU8(buf, 1);
		WkbU32(buf, 1015); // PolyhedralSurfaceZ
		WkbU32(buf, 6);
		WkbCubeFaces(buf, base, base + 2.0, /*reversed=*/false);
	}
	return buf;
}

static void ST_AsWKBMultiCubeFun(DataChunk &args, ExpressionState &state, Vector &result) {
	auto wkb = BuildMultiCubeWKB();
	auto blob_str = string_t(reinterpret_cast<const char *>(wkb.data()), wkb.size());
	result.SetVectorType(VectorType::CONSTANT_VECTOR);
	ConstantVector::GetData<string_t>(result)[0] = StringVector::AddStringOrBlob(result, blob_str);
}

static void ST_AsWKBMultiCubeSepFun(DataChunk &args, ExpressionState &state, Vector &result) {
	UnaryExecutor::Execute<double, string_t>(args.data[0], result, args.size(), [&](double separation) {
		auto wkb = BuildMultiCubeWKB(separation);
		auto blob_str = string_t(reinterpret_cast<const char *>(wkb.data()), wkb.size());
		return StringVector::AddStringOrBlob(result, blob_str);
	});
}

// Test helper: build a Point Z WKB from x, y, z.
static void ST_AsWKBPointZFun(DataChunk &args, ExpressionState &state, Vector &result) {
	TernaryExecutor::Execute<double, double, double, string_t>(
	    args.data[0], args.data[1], args.data[2], result, args.size(), [&](double x, double y, double z) {
		    std::vector<uint8_t> buf;
		    buf.push_back(1);     // little-endian
		    uint32_t type = 1001; // Point Z
		    buf.push_back(type & 0xFF);
		    buf.push_back((type >> 8) & 0xFF);
		    buf.push_back((type >> 16) & 0xFF);
		    buf.push_back((type >> 24) & 0xFF);
		    auto push_f64 = [&](double v) {
			    uint8_t b[8];
			    memcpy(b, &v, 8);
			    buf.insert(buf.end(), b, b + 8);
		    };
		    push_f64(x);
		    push_f64(y);
		    push_f64(z);
		    return StringVector::AddStringOrBlob(result,
		                                         string_t(reinterpret_cast<const char *>(buf.data()), buf.size()));
	    });
}

// Test helper: build a LineString Z WKB from (0,0,0) to (3,4,12).
static void ST_AsWKBLineZFun(DataChunk &args, ExpressionState &state, Vector &result) {
	std::vector<uint8_t> buf;
	auto push_u32 = [&](uint32_t v) {
		buf.push_back(v & 0xFF);
		buf.push_back((v >> 8) & 0xFF);
		buf.push_back((v >> 16) & 0xFF);
		buf.push_back((v >> 24) & 0xFF);
	};
	auto push_f64 = [&](double v) {
		uint8_t b[8];
		memcpy(b, &v, 8);
		buf.insert(buf.end(), b, b + 8);
	};
	buf.push_back(1); // little-endian
	push_u32(1002);   // LineString Z
	push_u32(2);
	push_f64(0.0);
	push_f64(0.0);
	push_f64(0.0);
	push_f64(3.0);
	push_f64(4.0);
	push_f64(12.0);

	result.SetVectorType(VectorType::CONSTANT_VECTOR);
	ConstantVector::GetData<string_t>(result)[0] =
	    StringVector::AddStringOrBlob(result, string_t(reinterpret_cast<const char *>(buf.data()), buf.size()));
}

// Test helper: build a MultiLineString Z with lengths 13 and 5.
static void ST_AsWKBMultiLineZFun(DataChunk &args, ExpressionState &state, Vector &result) {
	std::vector<uint8_t> buf;
	auto push_u32 = [&](uint32_t v) {
		buf.push_back(v & 0xFF);
		buf.push_back((v >> 8) & 0xFF);
		buf.push_back((v >> 16) & 0xFF);
		buf.push_back((v >> 24) & 0xFF);
	};
	auto push_f64 = [&](double v) {
		uint8_t b[8];
		memcpy(b, &v, 8);
		buf.insert(buf.end(), b, b + 8);
	};
	auto push_line = [&](double x0, double y0, double z0, double x1, double y1, double z1) {
		buf.push_back(1); // little-endian child
		push_u32(1002);   // LineString Z
		push_u32(2);
		push_f64(x0);
		push_f64(y0);
		push_f64(z0);
		push_f64(x1);
		push_f64(y1);
		push_f64(z1);
	};

	buf.push_back(1); // little-endian
	push_u32(1005);   // MultiLineString Z
	push_u32(2);
	push_line(0.0, 0.0, 0.0, 3.0, 4.0, 12.0);
	push_line(10.0, 10.0, 10.0, 13.0, 14.0, 10.0);

	result.SetVectorType(VectorType::CONSTANT_VECTOR);
	ConstantVector::GetData<string_t>(result)[0] =
	    StringVector::AddStringOrBlob(result, string_t(reinterpret_cast<const char *>(buf.data()), buf.size()));
}

// Test helper: build a Polygon Z rectangle (4x3) at z=5, single ring.
static void ST_AsWKBPolygonZFun(DataChunk &args, ExpressionState &state, Vector &result) {
	std::vector<uint8_t> buf;
	auto u32 = [&](uint32_t v) {
		buf.push_back(v & 0xFF);
		buf.push_back((v >> 8) & 0xFF);
		buf.push_back((v >> 16) & 0xFF);
		buf.push_back((v >> 24) & 0xFF);
	};
	auto f64 = [&](double v) {
		uint8_t b[8];
		memcpy(b, &v, 8);
		buf.insert(buf.end(), b, b + 8);
	};
	auto pt = [&](double x, double y, double z) {
		f64(x);
		f64(y);
		f64(z);
	};

	buf.push_back(1); // little-endian
	u32(1003);        // Polygon Z
	u32(1);           // 1 ring
	u32(5);           // 4 points + closing vertex
	pt(0, 0, 5);
	pt(4, 0, 5);
	pt(4, 3, 5);
	pt(0, 3, 5);
	pt(0, 0, 5);

	result.SetVectorType(VectorType::CONSTANT_VECTOR);
	ConstantVector::GetData<string_t>(result)[0] =
	    StringVector::AddStringOrBlob(result, string_t(reinterpret_cast<const char *>(buf.data()), buf.size()));
}

// Test helper: build a non-planar Polygon Z (one corner lifted in Z).
static void ST_AsWKBWarpedPolygonZFun(DataChunk &args, ExpressionState &state, Vector &result) {
	std::vector<uint8_t> buf;
	auto u32 = [&](uint32_t v) {
		buf.push_back(v & 0xFF);
		buf.push_back((v >> 8) & 0xFF);
		buf.push_back((v >> 16) & 0xFF);
		buf.push_back((v >> 24) & 0xFF);
	};
	auto f64 = [&](double v) {
		uint8_t b[8];
		memcpy(b, &v, 8);
		buf.insert(buf.end(), b, b + 8);
	};
	auto pt = [&](double x, double y, double z) {
		f64(x);
		f64(y);
		f64(z);
	};

	buf.push_back(1); // little-endian
	u32(1003);        // Polygon Z
	u32(1);           // 1 ring
	u32(5);           // 4 points + closing vertex
	pt(0, 0, 0);
	pt(2, 0, 0);
	pt(2, 2, 5);
	pt(0, 2, 0);
	pt(0, 0, 0);

	result.SetVectorType(VectorType::CONSTANT_VECTOR);
	ConstantVector::GetData<string_t>(result)[0] =
	    StringVector::AddStringOrBlob(result, string_t(reinterpret_cast<const char *>(buf.data()), buf.size()));
}

// Test helper: build a MultiPoint Z with 3 points (max z = 9).
static void ST_AsWKBMultiPointZFun(DataChunk &args, ExpressionState &state, Vector &result) {
	std::vector<uint8_t> buf;
	auto u32 = [&](uint32_t v) {
		buf.push_back(v & 0xFF);
		buf.push_back((v >> 8) & 0xFF);
		buf.push_back((v >> 16) & 0xFF);
		buf.push_back((v >> 24) & 0xFF);
	};
	auto f64 = [&](double v) {
		uint8_t b[8];
		memcpy(b, &v, 8);
		buf.insert(buf.end(), b, b + 8);
	};
	auto child_pt = [&](double x, double y, double z) {
		buf.push_back(1);
		u32(1001);
		f64(x);
		f64(y);
		f64(z);
	};

	buf.push_back(1); // little-endian
	u32(1004);        // MultiPoint Z
	u32(3);           // 3 points
	child_pt(1, 1, 1);
	child_pt(2, 2, 2);
	child_pt(3, 3, 9);

	result.SetVectorType(VectorType::CONSTANT_VECTOR);
	ConstantVector::GetData<string_t>(result)[0] =
	    StringVector::AddStringOrBlob(result, string_t(reinterpret_cast<const char *>(buf.data()), buf.size()));
}

// Test helper: build a MultiPolygon Z with two single-ring square faces.
static void ST_AsWKBMultiPolygonZFun(DataChunk &args, ExpressionState &state, Vector &result) {
	std::vector<uint8_t> buf;
	auto u32 = [&](uint32_t v) {
		buf.push_back(v & 0xFF);
		buf.push_back((v >> 8) & 0xFF);
		buf.push_back((v >> 16) & 0xFF);
		buf.push_back((v >> 24) & 0xFF);
	};
	auto f64 = [&](double v) {
		uint8_t b[8];
		memcpy(b, &v, 8);
		buf.insert(buf.end(), b, b + 8);
	};
	auto pt = [&](double x, double y, double z) {
		f64(x);
		f64(y);
		f64(z);
	};
	auto square = [&](double dx, double dy, double z) {
		buf.push_back(1);
		u32(1003);
		u32(1);
		u32(5); // child PolygonZ, 1 ring, 5 pts
		pt(dx, dy, z);
		pt(dx + 1, dy, z);
		pt(dx + 1, dy + 1, z);
		pt(dx, dy + 1, z);
		pt(dx, dy, z);
	};

	buf.push_back(1); // little-endian
	u32(1006);        // MultiPolygon Z
	u32(2);           // 2 polygons
	square(0, 0, 0);
	square(5, 5, 0);

	result.SetVectorType(VectorType::CONSTANT_VECTOR);
	ConstantVector::GetData<string_t>(result)[0] =
	    StringVector::AddStringOrBlob(result, string_t(reinterpret_cast<const char *>(buf.data()), buf.size()));
}

void RegisterFixtureFunctions(ExtensionLoader &loader) {
	// Test-only WKB fixture generators (st_aswkb*). Not part of the public
	// surface: registration is opt-in so the production catalog stays clean.
	// The SQL suite gets the variable from the Makefile's `export`; the .test
	// files that use fixtures declare `require-env THREE_D_TEST_FIXTURES`.
	if (std::getenv("THREE_D_TEST_FIXTURES") == nullptr) {
		return;
	}

	// Test helper: generate tetrahedron WKB
	loader.RegisterFunction(
	    ScalarFunction("st_aswkbpolyhedraltetra", {}, LogicalType::BLOB, ST_AsWKBPolyhedralTetraFun));
	loader.RegisterFunction(ScalarFunction("st_aswkbopentetra", {}, LogicalType::BLOB, ST_AsWKBOpenTetraFun));
	loader.RegisterFunction(ScalarFunction("st_aswkbhollowcube", {}, LogicalType::BLOB, ST_AsWKBHollowCubeFun));
	// Two overloads: the fixed-offset pair, and a separation-parameterised one
	// that pins volume conditioning for spatially separated parts.
	ScalarFunctionSet multicube_set("st_aswkbmulticube");
	multicube_set.AddFunction(ScalarFunction({}, LogicalType::BLOB, ST_AsWKBMultiCubeFun));
	multicube_set.AddFunction(ScalarFunction({LogicalType::DOUBLE}, LogicalType::BLOB, ST_AsWKBMultiCubeSepFun));
	loader.RegisterFunction(multicube_set);
	loader.RegisterFunction(ScalarFunction("st_aswkbpointz",
	                                       {LogicalType::DOUBLE, LogicalType::DOUBLE, LogicalType::DOUBLE},
	                                       LogicalType::BLOB, ST_AsWKBPointZFun));
	loader.RegisterFunction(ScalarFunction("st_aswkblinez", {}, LogicalType::BLOB, ST_AsWKBLineZFun));
	loader.RegisterFunction(ScalarFunction("st_aswkbmultilinez", {}, LogicalType::BLOB, ST_AsWKBMultiLineZFun));
	loader.RegisterFunction(ScalarFunction("st_aswkbpolygonz", {}, LogicalType::BLOB, ST_AsWKBPolygonZFun));
	loader.RegisterFunction(ScalarFunction("st_aswkbwarpedpolygonz", {}, LogicalType::BLOB, ST_AsWKBWarpedPolygonZFun));
	loader.RegisterFunction(ScalarFunction("st_aswkbmultipointz", {}, LogicalType::BLOB, ST_AsWKBMultiPointZFun));
	loader.RegisterFunction(ScalarFunction("st_aswkbmultipolygonz", {}, LogicalType::BLOB, ST_AsWKBMultiPolygonZFun));
}

} // namespace duckdb
