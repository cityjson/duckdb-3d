#define DUCKDB_EXTENSION_MAIN

#include "three_d_extension.hpp"
#include "duckdb.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/function/scalar_function.hpp"

#include "kernel/wkb_parser.hpp"
#include "kernel/model_builder.hpp"
#include "kernel/wkb_export.hpp"
#include "kernel/payload.hpp"
#include "kernel/measurements.hpp"
#include "kernel/triangulation.hpp"
#include "kernel/validation.hpp"
#include "kernel/metadata_parser.hpp"
#include "kernel/geom_model.hpp"
#include "kernel/geom_wkb_parser.hpp"
#include "kernel/geom_payload.hpp"
#include "kernel/geom_distance.hpp"
#include "kernel/geom_construct.hpp"
#include "kernel/geom_analysis.hpp"
#include "kernel/geom_serialize.hpp"
#include "kernel/crs_transform.hpp"
#include "kernel/arrow_native_import.hpp"
#include "duckdb/function/function_set.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/types/hugeint.hpp"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <unordered_map>

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
// geometry_properties {"shellCount":2,"shellFaceCounts":[6,6]} it imports as one
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
static std::vector<uint8_t> BuildMultiCubeWKB() {
	std::vector<uint8_t> buf;
	WkbU8(buf, 1);
	WkbU32(buf, 1007); // GeometryCollectionZ
	WkbU32(buf, 2);    // two members
	for (double base : {0.0, 5.0}) {
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

// ──────────────────────────────────────────────────────────────
// ST_3DFromWKB(wkb BLOB) → SOLID_3D (BLOB)
// ──────────────────────────────────────────────────────────────
static void ST_3DFromWKBFun(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &wkb_vec = args.data[0];
	UnaryExecutor::Execute<string_t, string_t>(wkb_vec, result, args.size(), [&](string_t wkb) {
		using namespace duckdb_3d;
		auto surfaces = ParseWKB(reinterpret_cast<const uint8_t *>(wkb.GetData()), wkb.GetSize());
		auto model = BuildSolidModel(surfaces);
		auto payload = SerializePayload(model);
		return StringVector::AddStringOrBlob(result,
		                                     string_t(reinterpret_cast<const char *>(payload.data()), payload.size()));
	});
}

// ──────────────────────────────────────────────────────────────
// ST_3DTryFromWKB(wkb BLOB) → SOLID_3D (BLOB) or NULL
// ──────────────────────────────────────────────────────────────
static void ST_3DTryFromWKBFun(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &wkb_vec = args.data[0];
	UnaryExecutor::ExecuteWithNulls<string_t, string_t>(
	    wkb_vec, result, args.size(), [&](string_t wkb, ValidityMask &mask, idx_t idx) -> string_t {
		    using namespace duckdb_3d;
		    try {
			    auto surfaces = ParseWKB(reinterpret_cast<const uint8_t *>(wkb.GetData()), wkb.GetSize());
			    auto model = BuildSolidModel(surfaces);
			    auto payload = SerializePayload(model);
			    return StringVector::AddStringOrBlob(
			        result, string_t(reinterpret_cast<const char *>(payload.data()), payload.size()));
		    } catch (...) {
			    mask.SetInvalid(idx);
			    return string_t();
		    }
	    });
}

// ──────────────────────────────────────────────────────────────
// ST_3DFromWKB(wkb BLOB, geometry_properties VARCHAR) → SOLID_3D
// ──────────────────────────────────────────────────────────────
static void ST_3DFromWKBWithMetaFun(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &wkb_vec = args.data[0];
	auto &meta_vec = args.data[1];
	auto count = args.size();

	UnifiedVectorFormat wkb_data, meta_data;
	wkb_vec.ToUnifiedFormat(count, wkb_data);
	meta_vec.ToUnifiedFormat(count, meta_data);
	auto wkb_strings = UnifiedVectorFormat::GetData<string_t>(wkb_data);
	auto meta_strings = UnifiedVectorFormat::GetData<string_t>(meta_data);
	auto &result_validity = FlatVector::Validity(result);

	for (idx_t i = 0; i < count; i++) {
		auto wkb_idx = wkb_data.sel->get_index(i);
		auto meta_idx = meta_data.sel->get_index(i);

		if (!wkb_data.validity.RowIsValid(wkb_idx)) {
			result_validity.SetInvalid(i);
			FlatVector::GetData<string_t>(result)[i] = string_t();
			continue;
		}

		using namespace duckdb_3d;
		auto &wkb = wkb_strings[wkb_idx];
		auto surfaces = ParseWKB(reinterpret_cast<const uint8_t *>(wkb.GetData()), wkb.GetSize());

		SolidModel model;
		if (meta_data.validity.RowIsValid(meta_idx)) {
			auto &meta_str = meta_strings[meta_idx];
			auto metadata = ParseGeometryProperties(std::string(meta_str.GetData(), meta_str.GetSize()));
			model = BuildSolidModel(surfaces, metadata);
		} else {
			model = BuildSolidModel(surfaces);
		}

		auto payload = SerializePayload(model);
		FlatVector::GetData<string_t>(result)[i] = StringVector::AddStringOrBlob(
		    result, string_t(reinterpret_cast<const char *>(payload.data()), payload.size()));
	}

	if (args.AllConstant()) {
		result.SetVectorType(VectorType::CONSTANT_VECTOR);
	}
}

// ──────────────────────────────────────────────────────────────
// ST_3DTryFromWKB(wkb BLOB, geometry_properties VARCHAR) → SOLID_3D or NULL
// ──────────────────────────────────────────────────────────────
static void ST_3DTryFromWKBWithMetaFun(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &wkb_vec = args.data[0];
	auto &meta_vec = args.data[1];
	auto count = args.size();

	UnifiedVectorFormat wkb_data, meta_data;
	wkb_vec.ToUnifiedFormat(count, wkb_data);
	meta_vec.ToUnifiedFormat(count, meta_data);
	auto wkb_strings = UnifiedVectorFormat::GetData<string_t>(wkb_data);
	auto meta_strings = UnifiedVectorFormat::GetData<string_t>(meta_data);
	auto &result_validity = FlatVector::Validity(result);

	for (idx_t i = 0; i < count; i++) {
		auto wkb_idx = wkb_data.sel->get_index(i);
		auto meta_idx = meta_data.sel->get_index(i);

		if (!wkb_data.validity.RowIsValid(wkb_idx)) {
			result_validity.SetInvalid(i);
			FlatVector::GetData<string_t>(result)[i] = string_t();
			continue;
		}

		try {
			using namespace duckdb_3d;
			auto &wkb = wkb_strings[wkb_idx];
			auto surfaces = ParseWKB(reinterpret_cast<const uint8_t *>(wkb.GetData()), wkb.GetSize());

			SolidModel model;
			if (meta_data.validity.RowIsValid(meta_idx)) {
				auto &meta_str = meta_strings[meta_idx];
				auto metadata = ParseGeometryProperties(std::string(meta_str.GetData(), meta_str.GetSize()));
				model = BuildSolidModel(surfaces, metadata);
			} else {
				model = BuildSolidModel(surfaces);
			}

			auto payload = SerializePayload(model);
			FlatVector::GetData<string_t>(result)[i] = StringVector::AddStringOrBlob(
			    result, string_t(reinterpret_cast<const char *>(payload.data()), payload.size()));
		} catch (...) {
			result_validity.SetInvalid(i);
			FlatVector::GetData<string_t>(result)[i] = string_t();
		}
	}

	if (args.AllConstant()) {
		result.SetVectorType(VectorType::CONSTANT_VECTOR);
	}
}

// ──────────────────────────────────────────────────────────────
// ST_3DFromWKB(wkb BLOB, geometry_properties STRUCT) → SOLID_3D
//
// A CityParquet package stores geometry_properties_lod* as a native STRUCT
// (spec §8), so we accept it directly rather than forcing a to_json() round-trip.
// Only `type` and `shells` are consumed (the same fields the JSON path reads);
// `surfaces` / `face_semantics` and any producer extras are ignored. The struct
// overload is registered as (BLOB, ANY) with a bind that normalises the struct
// and routes plain VARCHAR / JSON / SQLNULL metadata back to the JSON executor,
// so it is a strict superset of the VARCHAR overload.
// ──────────────────────────────────────────────────────────────
struct FromWkbStructBindData : public FunctionData {
	idx_t shells_index;
	bool has_type;
	idx_t type_index;

	FromWkbStructBindData(idx_t shells_index_p, bool has_type_p, idx_t type_index_p)
	    : shells_index(shells_index_p), has_type(has_type_p), type_index(type_index_p) {
	}

	unique_ptr<FunctionData> Copy() const override {
		return make_uniq<FromWkbStructBindData>(shells_index, has_type, type_index);
	}
	bool Equals(const FunctionData &other_p) const override {
		auto &o = other_p.Cast<FromWkbStructBindData>();
		return shells_index == o.shells_index && has_type == o.has_type && type_index == o.type_index;
	}
};

// Extract GeometryMetadata (type + shells) from row `row` of the flattened
// struct children. `shells_vec` is a normalised LIST(LIST(INTEGER)); `type_vec`
// (when present) is a normalised VARCHAR.
static duckdb_3d::GeometryMetadata ReadStructRowMetadata(Vector *type_vec, Vector *shells_vec, idx_t row,
                                                         bool has_type) {
	duckdb_3d::GeometryMetadata md;
	if (has_type && type_vec) {
		auto &type_validity = FlatVector::Validity(*type_vec);
		if (type_validity.RowIsValid(row)) {
			auto s = FlatVector::GetData<string_t>(*type_vec)[row];
			md.type = std::string(s.GetData(), s.GetSize());
		}
	}
	auto &shells_validity = FlatVector::Validity(*shells_vec);
	if (shells_validity.RowIsValid(row)) {
		auto outer = ListVector::GetData(*shells_vec)[row];
		auto &inner_list = ListVector::GetEntry(*shells_vec); // LIST(HUGEINT)
		auto inner_data = ListVector::GetData(inner_list);
		auto &int_vec = ListVector::GetEntry(inner_list); // HUGEINT
		auto int_data = FlatVector::GetData<hugeint_t>(int_vec);
		auto &int_validity = FlatVector::Validity(int_vec);
		auto &inner_list_validity = FlatVector::Validity(inner_list);
		std::vector<std::vector<uint32_t>> shells;
		shells.reserve(outer.length);
		for (idx_t j = 0; j < outer.length; j++) {
			// A present `shells` value carries no null nested elements (spec §8):
			// a null per-solid array is malformed input, not "no shells". Reject it
			// rather than reading whatever sits behind the null slot, whose contents
			// are unspecified rather than merely wrong. The face-count level below
			// is checked the same way.
			if (!inner_list_validity.RowIsValid(outer.offset + j)) {
				throw InvalidInputException(
				    "geometry_properties: null shells entry (no nested list element may be null)");
			}
			auto ie = inner_data[outer.offset + j];
			std::vector<uint32_t> shell;
			shell.reserve(ie.length);
			for (idx_t k = 0; k < ie.length; k++) {
				idx_t pos = ie.offset + k;
				if (!int_validity.RowIsValid(pos)) {
					throw InvalidInputException(
					    "geometry_properties: null shell face count (no nested list element may be null)");
				}
				// shells is normalised to HUGEINT[][] by the bind (so any standard
				// integer producer type is accepted without a pre-executor cast
				// failure); the fits-in-uint32 check runs here — inside the TRY
				// variant's catch — so an out-of-range count fails cleanly. TryCast
				// also rejects negatives.
				uint32_t face_count;
				if (!Hugeint::TryCast<uint32_t>(int_data[pos], face_count)) {
					throw InvalidInputException("geometry_properties: shells face count out of range");
				}
				shell.push_back(face_count);
			}
			shells.push_back(std::move(shell));
		}
		md.shells = std::move(shells);
	}
	return md;
}

// ──────────────────────────────────────────────────────────────
// Second geometry_properties STRUCT reader, for the ARROW-NATIVE path.
//
// ReadStructRowMetadata above serves ST_3DFromWKB's (BLOB, ANY) overload,
// whose bind normalises the struct first (shells -> HUGEINT[][], type ->
// VARCHAR), so it can read pre-normalised children by index. The
// arrow-native functions below bind the geometry_properties STRUCT
// directly, with no such normalisation pass, so they need a reader that
// walks the struct as the producer typed it (shells as INTEGER[][]) and
// flattens each nesting level itself. Keep both until the arrow-native
// overloads are moved onto the same (…, ANY) + bind strategy.
// ──────────────────────────────────────────────────────────────
// Adapter: geometry_properties STRUCT("type" VARCHAR, surfaces JSON,
// face_semantics INTEGER[], shells INTEGER[][]) row → the kernel's plain
// GeometryMetadata. duckdb-cityjson's arrow-native-type branch (commit
// d334b26) types geometry_properties_lod* this way instead of VARCHAR JSON
// text; this extracts the same shell-grouping information the JSON-text
// path parses. Deliberately kept here rather than in
// kernel/metadata_parser.* — it reads a live DuckDB Vector, so it belongs
// in the SQL/vectorized layer, not the DuckDB-free geometry kernel (mirrors
// how the BLOB WKB argument above is unwrapped into a plain uint8_t*/size
// before ParseWKB, rather than handing the kernel a Vector directly).
//
// `struct_vec` must already be flattened by the caller (`Vector::Flatten`) —
// a STRUCT argument can arrive as a CONSTANT_VECTOR (e.g. a literal, as in a
// simple `SELECT ... {...}` test query) whose children are not `FLAT_VECTOR`
// either; `FlatVector::GetData`/`IsNull` assert genuine flat vectors at
// every level, so flattening once up front (see
// ST_3DFromWKBWithStructMetaFun below) is required before this can index
// `row` directly through `StructVector`/`ListVector`. The nested `shells`
// list's own children (one level further down) are flattened inside this
// function itself, per-level, for the same reason.
// ──────────────────────────────────────────────────────────────

//! A literal or constant-folded expression (as in a simple `SELECT ...` test
//! query) can produce a non-FLAT_VECTOR at any nesting depth, not only the
//! top level — FlatVector::GetData/IsNull assert genuine flat vectors.
//! `count` is this level's own cardinality (its list's total element count
//! across every row, not the outer chunk's row count — list children are a
//! single vector shared/concatenated across all rows' entries).
static void FlattenIfNeeded(Vector &vec, idx_t count) {
	if (vec.GetVectorType() != VectorType::FLAT_VECTOR) {
		vec.Flatten(count);
	}
}

static duckdb_3d::GeometryMetadata ExtractGeometryPropertiesFromStruct(Vector &struct_vec, idx_t row) {
	duckdb_3d::GeometryMetadata result;
	auto &children = StructVector::GetEntries(struct_vec);
	// children[0] = type, children[1] = surfaces (unused for shell grouping,
	// same as the JSON-text parser), children[2] = face_semantics (unused),
	// children[3] = shells.
	auto &type_vec = *children[0];
	if (!FlatVector::IsNull(type_vec, row)) {
		result.type = FlatVector::GetData<string_t>(type_vec)[row].GetString();
	}

	auto &shells_vec = *children[3];
	if (FlatVector::IsNull(shells_vec, row)) {
		return result; // no shells -> non-solid type, same default as the JSON-text parser
	}
	auto outer_entry = FlatVector::GetData<list_entry_t>(shells_vec)[row];
	auto &inner_list_vec = ListVector::GetEntry(shells_vec);
	FlattenIfNeeded(inner_list_vec, ListVector::GetListSize(shells_vec));
	auto &inner_list_validity = FlatVector::Validity(inner_list_vec);
	for (idx_t solid_idx = outer_entry.offset; solid_idx < outer_entry.offset + outer_entry.length; solid_idx++) {
		// Nullability invariant (spec §8 / design doc): a present shells value
		// carries no null nested elements — a null per-solid array or a null
		// face-count within it is malformed input, not "no shells", so this
		// must be rejected rather than dereferencing whatever bytes happen to
		// sit behind the null slot (unspecified, not merely "wrong data").
		if (!inner_list_validity.RowIsValid(solid_idx)) {
			throw std::runtime_error("geometry_properties: null shells entry (no nested list element may be null)");
		}
		auto inner_entry = FlatVector::GetData<list_entry_t>(inner_list_vec)[solid_idx];
		auto &int_vec = ListVector::GetEntry(inner_list_vec);
		FlattenIfNeeded(int_vec, ListVector::GetListSize(inner_list_vec));
		auto &int_validity = FlatVector::Validity(int_vec);
		auto int_data = FlatVector::GetData<int32_t>(int_vec);
		std::vector<uint32_t> shell_face_counts;
		shell_face_counts.reserve(inner_entry.length);
		for (idx_t i = inner_entry.offset; i < inner_entry.offset + inner_entry.length; i++) {
			if (!int_validity.RowIsValid(i)) {
				throw std::runtime_error(
				    "geometry_properties: null shell face count (no nested list element may be null)");
			}
			if (int_data[i] < 0) {
				throw std::runtime_error("geometry_properties: expected non-negative shell face count");
			}
			shell_face_counts.push_back(static_cast<uint32_t>(int_data[i]));
		}
		result.shells.push_back(std::move(shell_face_counts));
	}
	return result;
}

static void ST_3DFromWKBWithStructFun(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &info = state.expr.Cast<BoundFunctionExpression>().bind_info->Cast<FromWkbStructBindData>();
	auto count = args.size();
	// Capture constness before Flatten mutates args.data[1]: a constant-folded
	// call must return a constant result (DuckDB asserts this in debug builds).
	bool all_constant = args.AllConstant();
	auto &wkb_vec = args.data[0];
	auto &meta_vec = args.data[1];

	UnifiedVectorFormat wkb_data;
	wkb_vec.ToUnifiedFormat(count, wkb_data);
	auto wkb_strings = UnifiedVectorFormat::GetData<string_t>(wkb_data);

	meta_vec.Flatten(count);
	auto &meta_validity = FlatVector::Validity(meta_vec);
	auto &children = StructVector::GetEntries(meta_vec);
	Vector *shells_vec = children[info.shells_index].get();
	shells_vec->Flatten(count);
	Vector *type_vec = nullptr;
	if (info.has_type) {
		type_vec = children[info.type_index].get();
		type_vec->Flatten(count);
	}

	auto &result_validity = FlatVector::Validity(result);
	auto result_data = FlatVector::GetData<string_t>(result);

	for (idx_t i = 0; i < count; i++) {
		auto wkb_idx = wkb_data.sel->get_index(i);
		if (!wkb_data.validity.RowIsValid(wkb_idx)) {
			result_validity.SetInvalid(i);
			result_data[i] = string_t();
			continue;
		}
		using namespace duckdb_3d;
		auto &wkb = wkb_strings[wkb_idx];
		auto surfaces = ParseWKB(reinterpret_cast<const uint8_t *>(wkb.GetData()), wkb.GetSize());

		SolidModel model;
		if (meta_validity.RowIsValid(i)) {
			auto md = ReadStructRowMetadata(type_vec, shells_vec, i, info.has_type);
			model = BuildSolidModel(surfaces, md);
		} else {
			model = BuildSolidModel(surfaces);
		}
		auto payload = SerializePayload(model);
		result_data[i] = StringVector::AddStringOrBlob(
		    result, string_t(reinterpret_cast<const char *>(payload.data()), payload.size()));
	}

	if (all_constant) {
		result.SetVectorType(VectorType::CONSTANT_VECTOR);
	}
}

static void ST_3DTryFromWKBWithStructFun(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &info = state.expr.Cast<BoundFunctionExpression>().bind_info->Cast<FromWkbStructBindData>();
	auto count = args.size();
	// Capture constness before Flatten mutates args.data[1]: a constant-folded
	// call must return a constant result (DuckDB asserts this in debug builds).
	bool all_constant = args.AllConstant();
	auto &wkb_vec = args.data[0];
	auto &meta_vec = args.data[1];

	UnifiedVectorFormat wkb_data;
	wkb_vec.ToUnifiedFormat(count, wkb_data);
	auto wkb_strings = UnifiedVectorFormat::GetData<string_t>(wkb_data);

	meta_vec.Flatten(count);
	auto &meta_validity = FlatVector::Validity(meta_vec);
	auto &children = StructVector::GetEntries(meta_vec);
	Vector *shells_vec = children[info.shells_index].get();
	shells_vec->Flatten(count);
	Vector *type_vec = nullptr;
	if (info.has_type) {
		type_vec = children[info.type_index].get();
		type_vec->Flatten(count);
	}

	auto &result_validity = FlatVector::Validity(result);
	auto result_data = FlatVector::GetData<string_t>(result);

	for (idx_t i = 0; i < count; i++) {
		auto wkb_idx = wkb_data.sel->get_index(i);
		if (!wkb_data.validity.RowIsValid(wkb_idx)) {
			result_validity.SetInvalid(i);
			result_data[i] = string_t();
			continue;
		}
		try {
			using namespace duckdb_3d;
			auto &wkb = wkb_strings[wkb_idx];
			auto surfaces = ParseWKB(reinterpret_cast<const uint8_t *>(wkb.GetData()), wkb.GetSize());

			SolidModel model;
			if (meta_validity.RowIsValid(i)) {
				auto md = ReadStructRowMetadata(type_vec, shells_vec, i, info.has_type);
				model = BuildSolidModel(surfaces, md);
			} else {
				model = BuildSolidModel(surfaces);
			}
			auto payload = SerializePayload(model);
			result_data[i] = StringVector::AddStringOrBlob(
			    result, string_t(reinterpret_cast<const char *>(payload.data()), payload.size()));
		} catch (...) {
			result_validity.SetInvalid(i);
			result_data[i] = string_t();
		}
	}

	if (all_constant) {
		result.SetVectorType(VectorType::CONSTANT_VECTOR);
	}
}

// Bind for the (BLOB, ANY) metadata overload. Routes SQLNULL / VARCHAR / JSON
// metadata to the JSON executor (`varchar_fn`) and a STRUCT to `struct_fn` after
// normalising the struct's `type` → VARCHAR and `shells` → LIST(LIST(INTEGER)).
static unique_ptr<FunctionData> BindWkbMetaAny(ScalarFunction &bound_function,
                                               vector<unique_ptr<Expression>> &arguments, scalar_function_t varchar_fn,
                                               scalar_function_t struct_fn) {
	auto &meta_type = arguments[1]->return_type;
	switch (meta_type.id()) {
	case LogicalTypeId::UNKNOWN:
		// A prepared-statement '?' parameter: defer to a later re-bind.
		throw ParameterNotResolvedException();
	case LogicalTypeId::SQLNULL:
	case LogicalTypeId::VARCHAR:
		// Plain / JSON-alias / NULL metadata: reuse the JSON executor unchanged.
		bound_function.arguments[1] = LogicalType::VARCHAR;
		bound_function.function = varchar_fn;
		return nullptr;
	case LogicalTypeId::STRUCT: {
		auto &child_types = StructType::GetChildTypes(meta_type);
		idx_t shells_index = DConstants::INVALID_INDEX;
		idx_t type_index = DConstants::INVALID_INDEX;
		bool has_type = false;
		child_list_t<LogicalType> normalized;
		normalized.reserve(child_types.size());
		for (idx_t i = 0; i < child_types.size(); i++) {
			auto &name = child_types[i].first;
			auto normalized_type = child_types[i].second;
			if (StringUtil::CIEquals(name, "shells")) {
				shells_index = i;
				// HUGEINT so every standard integer producer type (through UBIGINT)
				// widens without a bind-time cast failure; the executor range-checks
				// each value, so ST_3DTryFromWKB can turn an out-of-range count into
				// NULL instead of raising during the (pre-executor) struct cast.
				normalized_type = LogicalType::LIST(LogicalType::LIST(LogicalType::HUGEINT));
			} else if (StringUtil::CIEquals(name, "type")) {
				type_index = i;
				has_type = true;
				normalized_type = LogicalType::VARCHAR;
			}
			normalized.emplace_back(name, std::move(normalized_type));
		}
		if (shells_index == DConstants::INVALID_INDEX) {
			throw BinderException("ST_3DFromWKB: geometry_properties metadata STRUCT must contain a `shells` "
			                      "field; pass the metadata as a JSON VARCHAR otherwise");
		}
		bound_function.arguments[1] = LogicalType::STRUCT(std::move(normalized));
		bound_function.function = struct_fn;
		return make_uniq<FromWkbStructBindData>(shells_index, has_type, type_index);
	}
	default:
		throw BinderException("ST_3DFromWKB: metadata must be a geometry_properties STRUCT or a JSON VARCHAR, got " +
		                      meta_type.ToString());
	}
}

static unique_ptr<FunctionData> FromWkbAnyBind(ClientContext &, ScalarFunction &bound_function,
                                               vector<unique_ptr<Expression>> &arguments) {
	return BindWkbMetaAny(bound_function, arguments, ST_3DFromWKBWithMetaFun, ST_3DFromWKBWithStructFun);
}

static unique_ptr<FunctionData> TryFromWkbAnyBind(ClientContext &, ScalarFunction &bound_function,
                                                  vector<unique_ptr<Expression>> &arguments) {
	return BindWkbMetaAny(bound_function, arguments, ST_3DTryFromWKBWithMetaFun, ST_3DTryFromWKBWithStructFun);
}

// ──────────────────────────────────────────────────────────────
// ST_3DAsWKB(solid SOLID_3D) → BLOB
// ──────────────────────────────────────────────────────────────
static void ST_3DAsWKBFun(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &solid_vec = args.data[0];
	UnaryExecutor::Execute<string_t, string_t>(solid_vec, result, args.size(), [&](string_t solid) {
		using namespace duckdb_3d;
		auto model = DeserializePayload(reinterpret_cast<const uint8_t *>(solid.GetData()), solid.GetSize());
		auto wkb = ExportWKB(model);
		return StringVector::AddStringOrBlob(result, string_t(reinterpret_cast<const char *>(wkb.data()), wkb.size()));
	});
}

// ──────────────────────────────────────────────────────────────
// Introspection: ST_3DNumSolids, ST_3DNumShells, ST_3DNumFaces
// ──────────────────────────────────────────────────────────────
// These accessors only need element counts, which live in the fixed front
// header, so they read the header rather than deserialising the whole solid.
static void ST_3DNumSolidsFun(DataChunk &args, ExpressionState &state, Vector &result) {
	UnaryExecutor::Execute<string_t, int64_t>(args.data[0], result, args.size(), [](string_t solid) {
		using namespace duckdb_3d;
		auto info = ReadSolidPayloadHeader(reinterpret_cast<const uint8_t *>(solid.GetData()), solid.GetSize());
		return static_cast<int64_t>(info.solid_count);
	});
}

static void ST_3DNumShellsFun(DataChunk &args, ExpressionState &state, Vector &result) {
	UnaryExecutor::Execute<string_t, int64_t>(args.data[0], result, args.size(), [](string_t solid) {
		using namespace duckdb_3d;
		auto info = ReadSolidPayloadHeader(reinterpret_cast<const uint8_t *>(solid.GetData()), solid.GetSize());
		return static_cast<int64_t>(info.shell_count);
	});
}

static void ST_3DNumFacesFun(DataChunk &args, ExpressionState &state, Vector &result) {
	UnaryExecutor::Execute<string_t, int64_t>(args.data[0], result, args.size(), [](string_t solid) {
		using namespace duckdb_3d;
		auto info = ReadSolidPayloadHeader(reinterpret_cast<const uint8_t *>(solid.GetData()), solid.GetSize());
		return static_cast<int64_t>(info.face_count);
	});
}

// ──────────────────────────────────────────────────────────────
// Introspection: ST_3DBounds
// ──────────────────────────────────────────────────────────────
static void ST_3DBoundsFun(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &input = args.data[0];
	auto count = args.size();

	auto &entries = StructVector::GetEntries(result);
	auto &min_x_vec = *entries[0];
	auto &min_y_vec = *entries[1];
	auto &min_z_vec = *entries[2];
	auto &max_x_vec = *entries[3];
	auto &max_y_vec = *entries[4];
	auto &max_z_vec = *entries[5];

	UnifiedVectorFormat input_data;
	input.ToUnifiedFormat(count, input_data);

	auto input_strings = UnifiedVectorFormat::GetData<string_t>(input_data);
	auto &result_validity = FlatVector::Validity(result);

	for (idx_t i = 0; i < count; i++) {
		auto idx = input_data.sel->get_index(i);
		if (!input_data.validity.RowIsValid(idx)) {
			result_validity.SetInvalid(i);
			continue;
		}

		using namespace duckdb_3d;
		auto &blob = input_strings[idx];
		// Bounds live in the front header; no need to materialise the body.
		auto info = ReadSolidPayloadHeader(reinterpret_cast<const uint8_t *>(blob.GetData()), blob.GetSize());

		FlatVector::GetData<double>(min_x_vec)[i] = info.bbox.min_x;
		FlatVector::GetData<double>(min_y_vec)[i] = info.bbox.min_y;
		FlatVector::GetData<double>(min_z_vec)[i] = info.bbox.min_z;
		FlatVector::GetData<double>(max_x_vec)[i] = info.bbox.max_x;
		FlatVector::GetData<double>(max_y_vec)[i] = info.bbox.max_y;
		FlatVector::GetData<double>(max_z_vec)[i] = info.bbox.max_z;
	}
}

// ──────────────────────────────────────────────────────────────
// Validation: ST_3DIsClosed, ST_3DIsManifold, ST_3DIsOriented
// ──────────────────────────────────────────────────────────────
// The validation summary is cached in the payload (trailing block), so these
// read the header rather than re-running validation or parsing the body.
static void ST_3DIsClosedFun(DataChunk &args, ExpressionState &state, Vector &result) {
	UnaryExecutor::Execute<string_t, bool>(args.data[0], result, args.size(), [](string_t solid) {
		using namespace duckdb_3d;
		auto info = ReadSolidPayloadHeader(reinterpret_cast<const uint8_t *>(solid.GetData()), solid.GetSize());
		return info.validation.is_closed;
	});
}

static void ST_3DIsManifoldFun(DataChunk &args, ExpressionState &state, Vector &result) {
	UnaryExecutor::Execute<string_t, bool>(args.data[0], result, args.size(), [](string_t solid) {
		using namespace duckdb_3d;
		auto info = ReadSolidPayloadHeader(reinterpret_cast<const uint8_t *>(solid.GetData()), solid.GetSize());
		return info.validation.is_manifold;
	});
}

static void ST_3DIsOrientedFun(DataChunk &args, ExpressionState &state, Vector &result) {
	UnaryExecutor::Execute<string_t, bool>(args.data[0], result, args.size(), [](string_t solid) {
		using namespace duckdb_3d;
		auto info = ReadSolidPayloadHeader(reinterpret_cast<const uint8_t *>(solid.GetData()), solid.GetSize());
		return info.validation.is_oriented;
	});
}

// ──────────────────────────────────────────────────────────────
// ST_3DValidationReport
// ──────────────────────────────────────────────────────────────
static void ST_3DValidationReportFun(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &input = args.data[0];
	auto count = args.size();

	auto &entries = StructVector::GetEntries(result);
	auto &is_valid_vec = *entries[0];
	auto &is_closed_vec = *entries[1];
	auto &is_manifold_vec = *entries[2];
	auto &is_oriented_vec = *entries[3];
	auto &solid_count_vec = *entries[4];
	auto &shell_count_vec = *entries[5];
	auto &face_count_vec = *entries[6];
	auto &open_edge_vec = *entries[7];
	auto &non_manifold_vec = *entries[8];
	auto &degenerate_vec = *entries[9];
	auto &orientation_err_vec = *entries[10];
	auto &code_vec = *entries[11];
	auto &message_vec = *entries[12];

	UnifiedVectorFormat input_data;
	input.ToUnifiedFormat(count, input_data);
	auto input_strings = UnifiedVectorFormat::GetData<string_t>(input_data);
	auto &result_validity = FlatVector::Validity(result);

	for (idx_t i = 0; i < count; i++) {
		auto idx = input_data.sel->get_index(i);
		if (!input_data.validity.RowIsValid(idx)) {
			result_validity.SetInvalid(i);
			continue;
		}

		using namespace duckdb_3d;
		auto &blob = input_strings[idx];
		// Both the validation summary and counts are header/trailer data, so the
		// report is served without materialising vertices or topology.
		auto info = ReadSolidPayloadHeader(reinterpret_cast<const uint8_t *>(blob.GetData()), blob.GetSize());
		auto &vc = info.validation;

		FlatVector::GetData<bool>(is_valid_vec)[i] = vc.is_valid;
		FlatVector::GetData<bool>(is_closed_vec)[i] = vc.is_closed;
		FlatVector::GetData<bool>(is_manifold_vec)[i] = vc.is_manifold;
		FlatVector::GetData<bool>(is_oriented_vec)[i] = vc.is_oriented;
		FlatVector::GetData<int64_t>(solid_count_vec)[i] = info.solid_count;
		FlatVector::GetData<int64_t>(shell_count_vec)[i] = info.shell_count;
		FlatVector::GetData<int64_t>(face_count_vec)[i] = info.face_count;
		FlatVector::GetData<int64_t>(open_edge_vec)[i] = vc.open_edge_count;
		FlatVector::GetData<int64_t>(non_manifold_vec)[i] = vc.non_manifold_edge_count;
		FlatVector::GetData<int64_t>(degenerate_vec)[i] = vc.degenerate_face_count;
		FlatVector::GetData<int64_t>(orientation_err_vec)[i] = vc.orientation_error_count;

		// Generate code and message
		string code_str, msg_str;
		if (vc.is_valid) {
			code_str = "VALID";
			msg_str = "Valid solid";
		} else {
			std::vector<string> issues;
			if (!vc.is_closed)
				issues.push_back("not closed");
			if (!vc.is_manifold)
				issues.push_back("non-manifold edges");
			if (!vc.is_oriented)
				issues.push_back("orientation inconsistent");
			if (vc.degenerate_face_count > 0)
				issues.push_back("degenerate faces");
			code_str = "INVALID";
			msg_str = "Invalid solid: ";
			for (size_t j = 0; j < issues.size(); j++) {
				if (j > 0)
					msg_str += ", ";
				msg_str += issues[j];
			}
		}
		FlatVector::GetData<string_t>(code_vec)[i] = StringVector::AddString(code_vec, code_str);
		FlatVector::GetData<string_t>(message_vec)[i] = StringVector::AddString(message_vec, msg_str);
	}
}

// ──────────────────────────────────────────────────────────────
// Measurements: ST_3DSurfaceArea, ST_3DVolume
// ──────────────────────────────────────────────────────────────
static void ST_3DSurfaceAreaFun(DataChunk &args, ExpressionState &state, Vector &result) {
	UnaryExecutor::Execute<string_t, double>(args.data[0], result, args.size(), [](string_t solid) {
		using namespace duckdb_3d;
		auto model = DeserializePayload(reinterpret_cast<const uint8_t *>(solid.GetData()), solid.GetSize());
		if (model.TriangleCount() == 0) {
			TriangulateSolidModel(model);
		}
		return ComputeSurfaceArea(model);
	});
}

static void ST_3DVolumeFun(DataChunk &args, ExpressionState &state, Vector &result) {
	UnaryExecutor::Execute<string_t, double>(args.data[0], result, args.size(), [](string_t solid) {
		using namespace duckdb_3d;
		auto model = DeserializePayload(reinterpret_cast<const uint8_t *>(solid.GetData()), solid.GetSize());
		if (model.TriangleCount() == 0) {
			TriangulateSolidModel(model);
		}
		return ComputeVolume(model);
	});
}

static void ST_3DPerimeterFun(DataChunk &args, ExpressionState &state, Vector &result) {
	UnaryExecutor::Execute<string_t, double>(args.data[0], result, args.size(), [](string_t solid) {
		using namespace duckdb_3d;
		auto model = DeserializePayload(reinterpret_cast<const uint8_t *>(solid.GetData()), solid.GetSize());
		return ComputePerimeter(model);
	});
}

// ──────────────────────────────────────────────────────────────
// Accessors: ST_NDims
// ──────────────────────────────────────────────────────────────
static int32_t CoordinateDimension3D() {
	// v1 stores transformed XYZ coordinates only.
	return 3;
}

static void ST_NDimsFun(DataChunk &args, ExpressionState &state, Vector &result) {
	UnaryExecutor::Execute<string_t, int32_t>(args.data[0], result, args.size(),
	                                          [](string_t solid) { return CoordinateDimension3D(); });
}

static void ST_CoordDimFun(DataChunk &args, ExpressionState &state, Vector &result) {
	UnaryExecutor::Execute<string_t, int32_t>(args.data[0], result, args.size(),
	                                          [](string_t geom) { return CoordinateDimension3D(); });
}

static void ST_HasZFun(DataChunk &args, ExpressionState &state, Vector &result) {
	UnaryExecutor::Execute<string_t, bool>(args.data[0], result, args.size(), [](string_t solid) {
		// v1 geometries always carry a Z ordinate.
		return true;
	});
}

//! Peek at the payload magic to decide whether a BLOB is a SOLID_3D or GEOM_3D value.
enum class PayloadKind { Solid, Geom, Unknown };

static PayloadKind GetPayloadKind(const uint8_t *data, size_t size) {
	using namespace duckdb_3d;
	if (size >= 4) {
		if (std::memcmp(data, duckdb_3d::PAYLOAD_MAGIC, 4) == 0) {
			return PayloadKind::Solid;
		}
		if (std::memcmp(data, GEOM_PAYLOAD_MAGIC, 4) == 0) {
			return PayloadKind::Geom;
		}
	}
	return PayloadKind::Unknown;
}

static void ST_ZMinFun(DataChunk &args, ExpressionState &state, Vector &result) {
	UnaryExecutor::Execute<string_t, double>(args.data[0], result, args.size(), [](string_t blob) {
		using namespace duckdb_3d;
		auto data = reinterpret_cast<const uint8_t *>(blob.GetData());
		auto size = blob.GetSize();
		switch (GetPayloadKind(data, size)) {
		case PayloadKind::Solid:
			// bbox is in the front header — no body parse needed.
			return ReadSolidPayloadHeader(data, size).bbox.min_z;
		case PayloadKind::Geom:
			return ReadGeomPayloadHeader(data, size).bbox.min_z;
		default:
			throw InvalidInputException("ST_3DZMin: argument is not a SOLID_3D or GEOM_3D value");
		}
	});
}

static void ST_ZMaxFun(DataChunk &args, ExpressionState &state, Vector &result) {
	UnaryExecutor::Execute<string_t, double>(args.data[0], result, args.size(), [](string_t blob) {
		using namespace duckdb_3d;
		auto data = reinterpret_cast<const uint8_t *>(blob.GetData());
		auto size = blob.GetSize();
		switch (GetPayloadKind(data, size)) {
		case PayloadKind::Solid:
			return ReadSolidPayloadHeader(data, size).bbox.max_z;
		case PayloadKind::Geom:
			return ReadGeomPayloadHeader(data, size).bbox.max_z;
		default:
			throw InvalidInputException("ST_3DZMax: argument is not a SOLID_3D or GEOM_3D value");
		}
	});
}

// ST_3DFootprintArea accepts either a SOLID_3D (footprint of the solid) or a GEOM_3D
// (footprint of the geometry, e.g. a convex hull) — both the XY projection.
static void ST_AreaFun(DataChunk &args, ExpressionState &state, Vector &result) {
	UnaryExecutor::Execute<string_t, double>(args.data[0], result, args.size(), [](string_t blob) {
		using namespace duckdb_3d;
		auto data = reinterpret_cast<const uint8_t *>(blob.GetData());
		auto size = blob.GetSize();
		switch (GetPayloadKind(data, size)) {
		case PayloadKind::Solid:
			return ComputeFootprintArea(DeserializePayload(data, size));
		case PayloadKind::Geom:
			return Geom3DFootprintArea(DeserializeGeomPayload(data, size));
		default:
			throw InvalidInputException("ST_3DFootprintArea: argument is not a SOLID_3D or GEOM_3D value");
		}
	});
}

// ──────────────────────────────────────────────────────────────
// Transforms: ST_3DTranslate(solid SOLID_3D, dx, dy, dz DOUBLE) → SOLID_3D
// ──────────────────────────────────────────────────────────────
static void ST_TranslateFun(DataChunk &args, ExpressionState &state, Vector &result) {
	auto count = args.size();

	UnifiedVectorFormat solid_data, dx_data, dy_data, dz_data;
	args.data[0].ToUnifiedFormat(count, solid_data);
	args.data[1].ToUnifiedFormat(count, dx_data);
	args.data[2].ToUnifiedFormat(count, dy_data);
	args.data[3].ToUnifiedFormat(count, dz_data);

	auto solid_strings = UnifiedVectorFormat::GetData<string_t>(solid_data);
	auto dx_vals = UnifiedVectorFormat::GetData<double>(dx_data);
	auto dy_vals = UnifiedVectorFormat::GetData<double>(dy_data);
	auto dz_vals = UnifiedVectorFormat::GetData<double>(dz_data);
	auto &result_validity = FlatVector::Validity(result);

	for (idx_t i = 0; i < count; i++) {
		auto solid_idx = solid_data.sel->get_index(i);
		auto dx_idx = dx_data.sel->get_index(i);
		auto dy_idx = dy_data.sel->get_index(i);
		auto dz_idx = dz_data.sel->get_index(i);

		if (!solid_data.validity.RowIsValid(solid_idx) || !dx_data.validity.RowIsValid(dx_idx) ||
		    !dy_data.validity.RowIsValid(dy_idx) || !dz_data.validity.RowIsValid(dz_idx)) {
			result_validity.SetInvalid(i);
			FlatVector::GetData<string_t>(result)[i] = string_t();
			continue;
		}

		using namespace duckdb_3d;
		auto &blob = solid_strings[solid_idx];
		auto model = DeserializePayload(reinterpret_cast<const uint8_t *>(blob.GetData()), blob.GetSize());

		double dx = dx_vals[dx_idx], dy = dy_vals[dy_idx], dz = dz_vals[dz_idx];
		// Translation is a rigid motion: shift every vertex and the cached bbox;
		// topology, triangulation indices, and validation flags are unchanged.
		for (auto &v : model.vertices) {
			v.x += dx;
			v.y += dy;
			v.z += dz;
		}
		model.bbox.min_x += dx;
		model.bbox.max_x += dx;
		model.bbox.min_y += dy;
		model.bbox.max_y += dy;
		model.bbox.min_z += dz;
		model.bbox.max_z += dz;

		auto payload = SerializePayload(model);
		FlatVector::GetData<string_t>(result)[i] = StringVector::AddStringOrBlob(
		    result, string_t(reinterpret_cast<const char *>(payload.data()), payload.size()));
	}

	if (args.AllConstant()) {
		result.SetVectorType(VectorType::CONSTANT_VECTOR);
	}
}

// ──────────────────────────────────────────────────────────────
// Transforms: ST_3DTranslate(geom GEOM_3D, dx, dy, dz DOUBLE) → GEOM_3D
// ──────────────────────────────────────────────────────────────
static void ST_TranslateGeomFun(DataChunk &args, ExpressionState &state, Vector &result) {
	auto count = args.size();

	UnifiedVectorFormat geom_data, dx_data, dy_data, dz_data;
	args.data[0].ToUnifiedFormat(count, geom_data);
	args.data[1].ToUnifiedFormat(count, dx_data);
	args.data[2].ToUnifiedFormat(count, dy_data);
	args.data[3].ToUnifiedFormat(count, dz_data);

	auto geom_strings = UnifiedVectorFormat::GetData<string_t>(geom_data);
	auto dx_vals = UnifiedVectorFormat::GetData<double>(dx_data);
	auto dy_vals = UnifiedVectorFormat::GetData<double>(dy_data);
	auto dz_vals = UnifiedVectorFormat::GetData<double>(dz_data);
	auto &result_validity = FlatVector::Validity(result);

	for (idx_t i = 0; i < count; i++) {
		auto geom_idx = geom_data.sel->get_index(i);
		auto dx_idx = dx_data.sel->get_index(i);
		auto dy_idx = dy_data.sel->get_index(i);
		auto dz_idx = dz_data.sel->get_index(i);

		if (!geom_data.validity.RowIsValid(geom_idx) || !dx_data.validity.RowIsValid(dx_idx) ||
		    !dy_data.validity.RowIsValid(dy_idx) || !dz_data.validity.RowIsValid(dz_idx)) {
			result_validity.SetInvalid(i);
			FlatVector::GetData<string_t>(result)[i] = string_t();
			continue;
		}

		using namespace duckdb_3d;
		auto &blob = geom_strings[geom_idx];
		auto model = DeserializeGeomPayload(reinterpret_cast<const uint8_t *>(blob.GetData()), blob.GetSize());

		double dx = dx_vals[dx_idx], dy = dy_vals[dy_idx], dz = dz_vals[dz_idx];
		for (auto &v : model.vertices) {
			v.x += dx;
			v.y += dy;
			v.z += dz;
		}
		model.bbox.min_x += dx;
		model.bbox.max_x += dx;
		model.bbox.min_y += dy;
		model.bbox.max_y += dy;
		model.bbox.min_z += dz;
		model.bbox.max_z += dz;

		auto payload = SerializeGeomPayload(model);
		FlatVector::GetData<string_t>(result)[i] = StringVector::AddStringOrBlob(
		    result, string_t(reinterpret_cast<const char *>(payload.data()), payload.size()));
	}

	if (args.AllConstant()) {
		result.SetVectorType(VectorType::CONSTANT_VECTOR);
	}
}

// ──────────────────────────────────────────────────────────────
// Transforms: ST_3DScale(geom GEOM_3D, sx, sy, sz DOUBLE) → GEOM_3D
// ──────────────────────────────────────────────────────────────
static void ST_ScaleGeomFun(DataChunk &args, ExpressionState &state, Vector &result) {
	auto count = args.size();

	UnifiedVectorFormat geom_data, sx_data, sy_data, sz_data;
	args.data[0].ToUnifiedFormat(count, geom_data);
	args.data[1].ToUnifiedFormat(count, sx_data);
	args.data[2].ToUnifiedFormat(count, sy_data);
	args.data[3].ToUnifiedFormat(count, sz_data);

	auto geom_strings = UnifiedVectorFormat::GetData<string_t>(geom_data);
	auto sx_vals = UnifiedVectorFormat::GetData<double>(sx_data);
	auto sy_vals = UnifiedVectorFormat::GetData<double>(sy_data);
	auto sz_vals = UnifiedVectorFormat::GetData<double>(sz_data);
	auto &result_validity = FlatVector::Validity(result);

	for (idx_t i = 0; i < count; i++) {
		auto geom_idx = geom_data.sel->get_index(i);
		auto sx_idx = sx_data.sel->get_index(i);
		auto sy_idx = sy_data.sel->get_index(i);
		auto sz_idx = sz_data.sel->get_index(i);

		if (!geom_data.validity.RowIsValid(geom_idx) || !sx_data.validity.RowIsValid(sx_idx) ||
		    !sy_data.validity.RowIsValid(sy_idx) || !sz_data.validity.RowIsValid(sz_idx)) {
			result_validity.SetInvalid(i);
			FlatVector::GetData<string_t>(result)[i] = string_t();
			continue;
		}

		using namespace duckdb_3d;
		auto &blob = geom_strings[geom_idx];
		auto model = DeserializeGeomPayload(reinterpret_cast<const uint8_t *>(blob.GetData()), blob.GetSize());

		double sx = sx_vals[sx_idx], sy = sy_vals[sy_idx], sz = sz_vals[sz_idx];
		for (auto &v : model.vertices) {
			v.x *= sx;
			v.y *= sy;
			v.z *= sz;
		}
		model.ComputeBBox();

		auto payload = SerializeGeomPayload(model);
		FlatVector::GetData<string_t>(result)[i] = StringVector::AddStringOrBlob(
		    result, string_t(reinterpret_cast<const char *>(payload.data()), payload.size()));
	}

	if (args.AllConstant()) {
		result.SetVectorType(VectorType::CONSTANT_VECTOR);
	}
}

// ──────────────────────────────────────────────────────────────
// Transforms: ST_3DScale(solid SOLID_3D, sx, sy, sz DOUBLE) → SOLID_3D
// ──────────────────────────────────────────────────────────────
static void ST_ScaleFun(DataChunk &args, ExpressionState &state, Vector &result) {
	auto count = args.size();

	UnifiedVectorFormat solid_data, sx_data, sy_data, sz_data;
	args.data[0].ToUnifiedFormat(count, solid_data);
	args.data[1].ToUnifiedFormat(count, sx_data);
	args.data[2].ToUnifiedFormat(count, sy_data);
	args.data[3].ToUnifiedFormat(count, sz_data);

	auto solid_strings = UnifiedVectorFormat::GetData<string_t>(solid_data);
	auto sx_vals = UnifiedVectorFormat::GetData<double>(sx_data);
	auto sy_vals = UnifiedVectorFormat::GetData<double>(sy_data);
	auto sz_vals = UnifiedVectorFormat::GetData<double>(sz_data);
	auto &result_validity = FlatVector::Validity(result);

	for (idx_t i = 0; i < count; i++) {
		auto solid_idx = solid_data.sel->get_index(i);
		auto sx_idx = sx_data.sel->get_index(i);
		auto sy_idx = sy_data.sel->get_index(i);
		auto sz_idx = sz_data.sel->get_index(i);

		if (!solid_data.validity.RowIsValid(solid_idx) || !sx_data.validity.RowIsValid(sx_idx) ||
		    !sy_data.validity.RowIsValid(sy_idx) || !sz_data.validity.RowIsValid(sz_idx)) {
			result_validity.SetInvalid(i);
			FlatVector::GetData<string_t>(result)[i] = string_t();
			continue;
		}

		using namespace duckdb_3d;
		auto &blob = solid_strings[solid_idx];
		auto model = DeserializePayload(reinterpret_cast<const uint8_t *>(blob.GetData()), blob.GetSize());

		double sx = sx_vals[sx_idx], sy = sy_vals[sy_idx], sz = sz_vals[sz_idx];
		// Scale about the origin. Unlike a rigid motion, a degenerate (zero) factor
		// collapses faces and a negative factor changes geometry, so the cached
		// validation flags cannot simply carry over. Recompute validation from the
		// scaled coordinates (this catches the collapsed/degenerate case); the bbox
		// is recomputed too since negative factors can swap min/max.
		for (auto &v : model.vertices) {
			v.x *= sx;
			v.y *= sy;
			v.z *= sz;
		}
		model.ComputeBBox();
		ValidateSolidModel(model);

		auto payload = SerializePayload(model);
		FlatVector::GetData<string_t>(result)[i] = StringVector::AddStringOrBlob(
		    result, string_t(reinterpret_cast<const char *>(payload.data()), payload.size()));
	}

	if (args.AllConstant()) {
		result.SetVectorType(VectorType::CONSTANT_VECTOR);
	}
}

// ──────────────────────────────────────────────────────────────
// Transforms: ST_3DRotateX / ST_3DRotateY / ST_3DRotateZ(solid, radians) → SOLID_3D
// ──────────────────────────────────────────────────────────────
enum class RotationAxis { X, Y, Z };

static string_t RotateSolidBlob(Vector &result, string_t solid, double radians, RotationAxis axis) {
	using namespace duckdb_3d;
	auto model = DeserializePayload(reinterpret_cast<const uint8_t *>(solid.GetData()), solid.GetSize());

	double c = std::cos(radians);
	double s = std::sin(radians);
	// Right-handed, counter-clockwise rotation about the chosen axis (PostGIS
	// convention). A rigid motion: topology, winding, and validation flags are
	// preserved; only the bbox is recomputed.
	for (auto &v : model.vertices) {
		double x = v.x, y = v.y, z = v.z;
		switch (axis) {
		case RotationAxis::X:
			v.y = y * c - z * s;
			v.z = y * s + z * c;
			break;
		case RotationAxis::Y:
			v.x = x * c + z * s;
			v.z = -x * s + z * c;
			break;
		case RotationAxis::Z:
			v.x = x * c - y * s;
			v.y = x * s + y * c;
			break;
		}
	}
	model.ComputeBBox();

	auto payload = SerializePayload(model);
	return StringVector::AddStringOrBlob(result,
	                                     string_t(reinterpret_cast<const char *>(payload.data()), payload.size()));
}

static void ST_RotateXFun(DataChunk &args, ExpressionState &state, Vector &result) {
	BinaryExecutor::Execute<string_t, double, string_t>(
	    args.data[0], args.data[1], result, args.size(),
	    [&](string_t solid, double radians) { return RotateSolidBlob(result, solid, radians, RotationAxis::X); });
}

static void ST_RotateYFun(DataChunk &args, ExpressionState &state, Vector &result) {
	BinaryExecutor::Execute<string_t, double, string_t>(
	    args.data[0], args.data[1], result, args.size(),
	    [&](string_t solid, double radians) { return RotateSolidBlob(result, solid, radians, RotationAxis::Y); });
}

static void ST_RotateZFun(DataChunk &args, ExpressionState &state, Vector &result) {
	BinaryExecutor::Execute<string_t, double, string_t>(
	    args.data[0], args.data[1], result, args.size(),
	    [&](string_t solid, double radians) { return RotateSolidBlob(result, solid, radians, RotationAxis::Z); });
}

// ──────────────────────────────────────────────────────────────
// Transforms: ST_3DRotateX / ST_3DRotateY / ST_3DRotateZ(geom, radians) → GEOM_3D
// ──────────────────────────────────────────────────────────────
static string_t RotateGeomBlob(Vector &result, string_t geom, double radians, RotationAxis axis) {
	using namespace duckdb_3d;
	auto model = DeserializeGeomPayload(reinterpret_cast<const uint8_t *>(geom.GetData()), geom.GetSize());

	double c = std::cos(radians);
	double s = std::sin(radians);
	for (auto &v : model.vertices) {
		double x = v.x, y = v.y, z = v.z;
		switch (axis) {
		case RotationAxis::X:
			v.y = y * c - z * s;
			v.z = y * s + z * c;
			break;
		case RotationAxis::Y:
			v.x = x * c + z * s;
			v.z = -x * s + z * c;
			break;
		case RotationAxis::Z:
			v.x = x * c - y * s;
			v.y = x * s + y * c;
			break;
		}
	}
	model.ComputeBBox();

	auto payload = SerializeGeomPayload(model);
	return StringVector::AddStringOrBlob(result,
	                                     string_t(reinterpret_cast<const char *>(payload.data()), payload.size()));
}

static void ST_RotateXGeomFun(DataChunk &args, ExpressionState &state, Vector &result) {
	BinaryExecutor::Execute<string_t, double, string_t>(
	    args.data[0], args.data[1], result, args.size(),
	    [&](string_t geom, double radians) { return RotateGeomBlob(result, geom, radians, RotationAxis::X); });
}

static void ST_RotateYGeomFun(DataChunk &args, ExpressionState &state, Vector &result) {
	BinaryExecutor::Execute<string_t, double, string_t>(
	    args.data[0], args.data[1], result, args.size(),
	    [&](string_t geom, double radians) { return RotateGeomBlob(result, geom, radians, RotationAxis::Y); });
}

static void ST_RotateZGeomFun(DataChunk &args, ExpressionState &state, Vector &result) {
	BinaryExecutor::Execute<string_t, double, string_t>(
	    args.data[0], args.data[1], result, args.size(),
	    [&](string_t geom, double radians) { return RotateGeomBlob(result, geom, radians, RotationAxis::Z); });
}

// ──────────────────────────────────────────────────────────────
// GEOM_3D: general geometry construction and accessors
// ──────────────────────────────────────────────────────────────

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

// ST_Geom3DFromWKB(wkb BLOB) → GEOM_3D
// (named to avoid clashing with DuckDB core's st_geomfromwkb -> GEOMETRY)
static void ST_Geom3DFromWKBFun(DataChunk &args, ExpressionState &state, Vector &result) {
	UnaryExecutor::Execute<string_t, string_t>(args.data[0], result, args.size(), [&](string_t wkb) {
		using namespace duckdb_3d;
		auto model = ParseGeomWKB(reinterpret_cast<const uint8_t *>(wkb.GetData()), wkb.GetSize());
		auto payload = SerializeGeomPayload(model);
		return StringVector::AddStringOrBlob(result,
		                                     string_t(reinterpret_cast<const char *>(payload.data()), payload.size()));
	});
}

static const char *GeomTypeName(duckdb_3d::GeomType type) {
	using namespace duckdb_3d;
	switch (type) {
	case GeomType::Point:
		return "ST_Point";
	case GeomType::LineString:
		return "ST_LineString";
	case GeomType::Polygon:
		return "ST_Polygon";
	case GeomType::MultiPoint:
		return "ST_MultiPoint";
	case GeomType::MultiLineString:
		return "ST_MultiLineString";
	case GeomType::MultiPolygon:
		return "ST_MultiPolygon";
	case GeomType::GeometryCollection:
		return "ST_GeometryCollection";
	case GeomType::PolyhedralSurface:
		return "ST_PolyhedralSurface";
	default:
		return "ST_Geometry";
	}
}

// ST_3DGeometryType(geom GEOM_3D) → VARCHAR
static void ST_GeometryTypeFun(DataChunk &args, ExpressionState &state, Vector &result) {
	UnaryExecutor::Execute<string_t, string_t>(args.data[0], result, args.size(), [&](string_t geom) {
		using namespace duckdb_3d;
		auto model = DeserializeGeomPayload(reinterpret_cast<const uint8_t *>(geom.GetData()), geom.GetSize());
		return StringVector::AddString(result, GeomTypeName(model.type));
	});
}

// Point ordinate accessors: ST_3DX / ST_3DY / ST_3DZ(point GEOM_3D) → DOUBLE.
enum class Ordinate { X, Y, Z };

static double PointOrdinate(string_t geom, Ordinate ord) {
	using namespace duckdb_3d;
	auto model = DeserializeGeomPayload(reinterpret_cast<const uint8_t *>(geom.GetData()), geom.GetSize());
	if (model.type != GeomType::Point) {
		throw InvalidInputException("ST_3DX/ST_3DY/ST_3DZ: argument is not a Point");
	}
	if (model.vertices.empty()) {
		throw InvalidInputException("ST_3DX/ST_3DY/ST_3DZ: empty point");
	}
	const auto &v = model.vertices[0];
	switch (ord) {
	case Ordinate::X:
		return v.x;
	case Ordinate::Y:
		return v.y;
	default:
		return v.z;
	}
}

static void ST_XFun(DataChunk &args, ExpressionState &state, Vector &result) {
	UnaryExecutor::Execute<string_t, double>(args.data[0], result, args.size(),
	                                         [](string_t geom) { return PointOrdinate(geom, Ordinate::X); });
}
static void ST_YFun(DataChunk &args, ExpressionState &state, Vector &result) {
	UnaryExecutor::Execute<string_t, double>(args.data[0], result, args.size(),
	                                         [](string_t geom) { return PointOrdinate(geom, Ordinate::Y); });
}
static void ST_ZFun(DataChunk &args, ExpressionState &state, Vector &result) {
	UnaryExecutor::Execute<string_t, double>(args.data[0], result, args.size(),
	                                         [](string_t geom) { return PointOrdinate(geom, Ordinate::Z); });
}

static double Geom3DLength(string_t geom) {
	using namespace duckdb_3d;
	auto model = DeserializeGeomPayload(reinterpret_cast<const uint8_t *>(geom.GetData()), geom.GetSize());
	auto sum_segments = [&](size_t begin, size_t end) {
		double length = 0.0;
		for (size_t i = begin + 1; i < end; i++) {
			const auto &a = model.vertices[i - 1];
			const auto &b = model.vertices[i];
			double dx = b.x - a.x;
			double dy = b.y - a.y;
			double dz = b.z - a.z;
			length += std::sqrt(dx * dx + dy * dy + dz * dz);
		}
		return length;
	};

	if (model.type == GeomType::LineString) {
		return sum_segments(0, model.vertices.size());
	}
	if (model.type == GeomType::MultiLineString) {
		double length = 0.0;
		for (size_t part = 1; part < model.part_offsets.size(); part++) {
			size_t begin = model.part_offsets[part - 1];
			size_t end = model.part_offsets[part];
			length += sum_segments(begin, end);
		}
		return length;
	}
	return 0.0;
}

static void ST_3DLengthFun(DataChunk &args, ExpressionState &state, Vector &result) {
	UnaryExecutor::Execute<string_t, double>(args.data[0], result, args.size(),
	                                         [](string_t geom) { return Geom3DLength(geom); });
}

// ST_3DDistance(g1 GEOM_3D, g2 GEOM_3D) → DOUBLE
static double Geom3DDistanceSQL(string_t g1, string_t g2) {
	using namespace duckdb_3d;
	auto m1 = DeserializeGeomPayload(reinterpret_cast<const uint8_t *>(g1.GetData()), g1.GetSize());
	auto m2 = DeserializeGeomPayload(reinterpret_cast<const uint8_t *>(g2.GetData()), g2.GetSize());
	return Geom3DDistance(m1, m2);
}

static void ST_3DDistanceFun(DataChunk &args, ExpressionState &state, Vector &result) {
	BinaryExecutor::Execute<string_t, string_t, double>(
	    args.data[0], args.data[1], result, args.size(),
	    [](string_t g1, string_t g2) { return Geom3DDistanceSQL(g1, g2); });
}

// ST_3DDWithin(g1 GEOM_3D, g2 GEOM_3D, dist DOUBLE) → BOOLEAN
static bool Geom3DWithinSQL(string_t g1, string_t g2, double dist) {
	using namespace duckdb_3d;
	auto m1 = DeserializeGeomPayload(reinterpret_cast<const uint8_t *>(g1.GetData()), g1.GetSize());
	auto m2 = DeserializeGeomPayload(reinterpret_cast<const uint8_t *>(g2.GetData()), g2.GetSize());
	return Geom3DWithin(m1, m2, dist);
}

static void ST_3DDWithinFun(DataChunk &args, ExpressionState &state, Vector &result) {
	TernaryExecutor::Execute<string_t, string_t, double, bool>(args.data[0], args.data[1], args.data[2], result,
	                                                           args.size(), [](string_t g1, string_t g2, double dist) {
		                                                           if (dist < 0.0) {
			                                                           return false;
		                                                           }
		                                                           // Uses bbox pruning + first-hit early exit instead
		                                                           // of the exact distance.
		                                                           return Geom3DWithinSQL(g1, g2, dist);
	                                                           });
}

// ST_3DExtrude(polygon GEOM_3D, height DOUBLE) → SOLID_3D
static void ST_3DExtrudeFun(DataChunk &args, ExpressionState &state, Vector &result) {
	BinaryExecutor::Execute<string_t, double, string_t>(
	    args.data[0], args.data[1], result, args.size(), [&](string_t geom, double height) {
		    using namespace duckdb_3d;
		    auto poly = DeserializeGeomPayload(reinterpret_cast<const uint8_t *>(geom.GetData()), geom.GetSize());
		    auto solid = BuildExtrudedSolid(poly, height);
		    auto payload = SerializePayload(solid);
		    return StringVector::AddStringOrBlob(
		        result, string_t(reinterpret_cast<const char *>(payload.data()), payload.size()));
	    });
}

// ST_MakeSolid(surface GEOM_3D) → SOLID_3D (BLOB)
static void ST_MakeSolidFun(DataChunk &args, ExpressionState &state, Vector &result) {
	UnaryExecutor::Execute<string_t, string_t>(args.data[0], result, args.size(), [&](string_t geom) {
		using namespace duckdb_3d;
		auto surface = DeserializeGeomPayload(reinterpret_cast<const uint8_t *>(geom.GetData()), geom.GetSize());
		auto solid = BuildSolidFromSurface(surface);
		auto payload = SerializePayload(solid);
		return StringVector::AddStringOrBlob(result,
		                                     string_t(reinterpret_cast<const char *>(payload.data()), payload.size()));
	});
}

// ST_3DMaxDistance(g1 GEOM_3D, g2 GEOM_3D) → DOUBLE
static double Geom3DMaxDistanceSQL(string_t g1, string_t g2) {
	using namespace duckdb_3d;
	auto m1 = DeserializeGeomPayload(reinterpret_cast<const uint8_t *>(g1.GetData()), g1.GetSize());
	auto m2 = DeserializeGeomPayload(reinterpret_cast<const uint8_t *>(g2.GetData()), g2.GetSize());
	return Geom3DMaxDistance(m1, m2);
}

static void ST_3DMaxDistanceFun(DataChunk &args, ExpressionState &state, Vector &result) {
	BinaryExecutor::Execute<string_t, string_t, double>(
	    args.data[0], args.data[1], result, args.size(),
	    [](string_t g1, string_t g2) { return Geom3DMaxDistanceSQL(g1, g2); });
}

// ST_3DDFullyWithin(g1 GEOM_3D, g2 GEOM_3D, dist DOUBLE) → BOOLEAN
static void ST_3DDFullyWithinFun(DataChunk &args, ExpressionState &state, Vector &result) {
	TernaryExecutor::Execute<string_t, string_t, double, bool>(args.data[0], args.data[1], args.data[2], result,
	                                                           args.size(), [](string_t g1, string_t g2, double dist) {
		                                                           if (dist < 0.0) {
			                                                           return false;
		                                                           }
		                                                           return Geom3DMaxDistanceSQL(g1, g2) <= dist;
	                                                           });
}

// ST_3DIntersects(g1 GEOM_3D, g2 GEOM_3D) → BOOLEAN
// Two geometries intersect when their minimum 3D distance is zero (touching
// counts as intersecting), tested within a small tolerance.
static void ST_3DIntersectsFun(DataChunk &args, ExpressionState &state, Vector &result) {
	constexpr double kIntersectEps = 1e-9;
	BinaryExecutor::Execute<string_t, string_t, bool>(
	    args.data[0], args.data[1], result, args.size(),
	    [&](string_t g1, string_t g2) { return Geom3DWithinSQL(g1, g2, kIntersectEps); });
}

// ST_3DClosestPoint(g1 GEOM_3D, g2 GEOM_3D) → GEOM_3D (Point)
// Returns the point on g1 that is closest to g2.
static void ST_3DClosestPointFun(DataChunk &args, ExpressionState &state, Vector &result) {
	BinaryExecutor::Execute<string_t, string_t, string_t>(
	    args.data[0], args.data[1], result, args.size(), [&](string_t g1, string_t g2) {
		    using namespace duckdb_3d;
		    auto m1 = DeserializeGeomPayload(reinterpret_cast<const uint8_t *>(g1.GetData()), g1.GetSize());
		    auto m2 = DeserializeGeomPayload(reinterpret_cast<const uint8_t *>(g2.GetData()), g2.GetSize());
		    auto pair = Geom3DClosestPoints(m1, m2);
		    GeomModel point;
		    point.type = GeomType::Point;
		    point.vertices.push_back(pair.p);
		    point.ComputeBBox();
		    auto payload = SerializeGeomPayload(point);
		    return StringVector::AddStringOrBlob(
		        result, string_t(reinterpret_cast<const char *>(payload.data()), payload.size()));
	    });
}

// ST_3DShortestLine(g1 GEOM_3D, g2 GEOM_3D) → GEOM_3D (LineString)
// Returns the shortest line segment connecting g1 and g2.
static void ST_3DShortestLineFun(DataChunk &args, ExpressionState &state, Vector &result) {
	BinaryExecutor::Execute<string_t, string_t, string_t>(
	    args.data[0], args.data[1], result, args.size(), [&](string_t g1, string_t g2) {
		    using namespace duckdb_3d;
		    auto m1 = DeserializeGeomPayload(reinterpret_cast<const uint8_t *>(g1.GetData()), g1.GetSize());
		    auto m2 = DeserializeGeomPayload(reinterpret_cast<const uint8_t *>(g2.GetData()), g2.GetSize());
		    auto pair = Geom3DClosestPoints(m1, m2);
		    GeomModel line;
		    line.type = GeomType::LineString;
		    line.vertices = {pair.p, pair.q};
		    line.ComputeBBox();
		    auto payload = SerializeGeomPayload(line);
		    return StringVector::AddStringOrBlob(
		        result, string_t(reinterpret_cast<const char *>(payload.data()), payload.size()));
	    });
}

// ST_3DAsText(geom GEOM_3D) → VARCHAR
static void ST_AsTextFun(DataChunk &args, ExpressionState &state, Vector &result) {
	UnaryExecutor::Execute<string_t, string_t>(args.data[0], result, args.size(), [&](string_t geom) {
		using namespace duckdb_3d;
		auto model = DeserializeGeomPayload(reinterpret_cast<const uint8_t *>(geom.GetData()), geom.GetSize());
		auto text = Geom3DAsText(model);
		return StringVector::AddString(result, text);
	});
}

// ST_3DAsGeoJSON(geom GEOM_3D) → VARCHAR
static void ST_AsGeoJSONFun(DataChunk &args, ExpressionState &state, Vector &result) {
	UnaryExecutor::Execute<string_t, string_t>(args.data[0], result, args.size(), [&](string_t geom) {
		using namespace duckdb_3d;
		auto model = DeserializeGeomPayload(reinterpret_cast<const uint8_t *>(geom.GetData()), geom.GetSize());
		auto text = Geom3DAsGeoJSON(model);
		return StringVector::AddString(result, text);
	});
}

// ST_3DAsBinary(geom GEOM_3D) → BLOB
static void ST_AsBinaryFun(DataChunk &args, ExpressionState &state, Vector &result) {
	UnaryExecutor::Execute<string_t, string_t>(args.data[0], result, args.size(), [&](string_t geom) {
		using namespace duckdb_3d;
		auto model = DeserializeGeomPayload(reinterpret_cast<const uint8_t *>(geom.GetData()), geom.GetSize());
		auto binary = Geom3DAsBinary(model);
		return StringVector::AddStringOrBlob(result,
		                                     string_t(reinterpret_cast<const char *>(binary.data()), binary.size()));
	});
}

// ST_IsPlanar(geom GEOM_3D) → BOOLEAN
static void ST_IsPlanarFun(DataChunk &args, ExpressionState &state, Vector &result) {
	UnaryExecutor::Execute<string_t, bool>(args.data[0], result, args.size(), [](string_t geom) {
		using namespace duckdb_3d;
		auto model = DeserializeGeomPayload(reinterpret_cast<const uint8_t *>(geom.GetData()), geom.GetSize());
		return Geom3DIsPlanar(model);
	});
}

// ST_3DCentroid(geom GEOM_3D) → GEOM_3D (Point)
static void ST_3DCentroidFun(DataChunk &args, ExpressionState &state, Vector &result) {
	UnaryExecutor::Execute<string_t, string_t>(args.data[0], result, args.size(), [&](string_t geom) {
		using namespace duckdb_3d;
		auto model = DeserializeGeomPayload(reinterpret_cast<const uint8_t *>(geom.GetData()), geom.GetSize());
		auto centroid = Geom3DCentroid(model);
		GeomModel point;
		point.type = GeomType::Point;
		point.vertices.push_back(centroid);
		point.ComputeBBox();
		auto payload = SerializeGeomPayload(point);
		return StringVector::AddStringOrBlob(result,
		                                     string_t(reinterpret_cast<const char *>(payload.data()), payload.size()));
	});
}

// ST_Force3D(geom GEOM_3D) → GEOM_3D
// GEOM_3D already stores XYZ, so this is currently an identity cast; future 2D
// inputs would have Z set to 0 here.
static void ST_Force3DFun(DataChunk &args, ExpressionState &state, Vector &result) {
	UnaryExecutor::Execute<string_t, string_t>(args.data[0], result, args.size(), [&](string_t geom) {
		using namespace duckdb_3d;
		auto model = DeserializeGeomPayload(reinterpret_cast<const uint8_t *>(geom.GetData()), geom.GetSize());
		auto payload = SerializeGeomPayload(model);
		return StringVector::AddStringOrBlob(result,
		                                     string_t(reinterpret_cast<const char *>(payload.data()), payload.size()));
	});
}

// ST_3DConvexHull(geom GEOM_3D) → GEOM_3D
// 2D monotone-chain hull over XY-projected vertices; output Z = input min Z.
static void ST_ConvexHullFun(DataChunk &args, ExpressionState &state, Vector &result) {
	UnaryExecutor::Execute<string_t, string_t>(args.data[0], result, args.size(), [&](string_t geom) {
		using namespace duckdb_3d;
		auto model = DeserializeGeomPayload(reinterpret_cast<const uint8_t *>(geom.GetData()), geom.GetSize());
		auto hull = Geom3DConvexHull(model);
		auto payload = SerializeGeomPayload(hull);
		return StringVector::AddStringOrBlob(result,
		                                     string_t(reinterpret_cast<const char *>(payload.data()), payload.size()));
	});
}

// ST_3DDimension(geom GEOM_3D) → INTEGER
static int32_t GeomDimension(duckdb_3d::GeomType type) {
	using namespace duckdb_3d;
	switch (type) {
	case GeomType::Point:
	case GeomType::MultiPoint:
		return 0;
	case GeomType::LineString:
	case GeomType::MultiLineString:
		return 1;
	case GeomType::Polygon:
	case GeomType::MultiPolygon:
	case GeomType::PolyhedralSurface:
	case GeomType::GeometryCollection:
		return 2;
	default:
		return 2;
	}
}

static void ST_DimensionFun(DataChunk &args, ExpressionState &state, Vector &result) {
	UnaryExecutor::Execute<string_t, int32_t>(args.data[0], result, args.size(), [](string_t geom) {
		using namespace duckdb_3d;
		auto model = DeserializeGeomPayload(reinterpret_cast<const uint8_t *>(geom.GetData()), geom.GetSize());
		return GeomDimension(model.type);
	});
}

// ST_3DNumGeometries(geom GEOM_3D) → INTEGER
static int32_t GeomNumGeometries(const duckdb_3d::GeomModel &model) {
	using namespace duckdb_3d;
	switch (model.type) {
	case GeomType::MultiPoint:
	case GeomType::MultiLineString:
	case GeomType::MultiPolygon:
	case GeomType::GeometryCollection:
		return model.part_offsets.empty() ? 0 : static_cast<int32_t>(model.part_offsets.size() - 1);
	default:
		return 1;
	}
}

static void ST_NumGeometriesFun(DataChunk &args, ExpressionState &state, Vector &result) {
	UnaryExecutor::Execute<string_t, int32_t>(args.data[0], result, args.size(), [](string_t geom) {
		using namespace duckdb_3d;
		auto model = DeserializeGeomPayload(reinterpret_cast<const uint8_t *>(geom.GetData()), geom.GetSize());
		return GeomNumGeometries(model);
	});
}

// ──────────────────────────────────────────────────────────────
// Transforms: ST_3DTransform — 2D CRS reprojection (X/Y only, Z preserved).
// Accepts SOLID_3D or GEOM_3D (dispatched by payload magic); output type
// equals input type. PROJ is confined to kernel/crs_transform.
// ──────────────────────────────────────────────────────────────

//! Reproject one payload BLOB using an already-built transform. Recomputes the
//! bbox; re-validates solids because reprojection can invert winding.
static std::vector<uint8_t> ReprojectPayloadBlob(const uint8_t *data, size_t size, const duckdb_3d::CrsTransform &tf) {
	using namespace duckdb_3d;
	switch (GetPayloadKind(data, size)) {
	case PayloadKind::Solid: {
		auto model = DeserializePayload(data, size);
		tf.ReprojectXY(model.vertices);
		model.ComputeBBox();
		ValidateSolidModel(model);
		return SerializePayload(model);
	}
	case PayloadKind::Geom: {
		auto model = DeserializeGeomPayload(data, size);
		tf.ReprojectXY(model.vertices);
		model.ComputeBBox();
		return SerializeGeomPayload(model);
	}
	default:
		throw InvalidInputException("ST_3DTransform: argument is not a SOLID_3D or GEOM_3D value");
	}
}

//! Shared chunk loop. `get_crs(i)` returns the {source, target} CRS strings for
//! row i. One CrsTransform is built per distinct pair and reused across rows.
template <class GetCrs>
static void TransformChunk(DataChunk &args, Vector &result, GetCrs get_crs) {
	using namespace duckdb_3d;
	auto count = args.size();

	UnifiedVectorFormat geom_data;
	args.data[0].ToUnifiedFormat(count, geom_data);
	auto geom_strings = UnifiedVectorFormat::GetData<string_t>(geom_data);
	auto &result_validity = FlatVector::Validity(result);

	std::unordered_map<std::string, std::unique_ptr<CrsTransform>> cache;

	for (idx_t i = 0; i < count; i++) {
		auto geom_idx = geom_data.sel->get_index(i);

		bool crs_valid = true;
		std::string source;
		std::string target;
		if (!geom_data.validity.RowIsValid(geom_idx) || !get_crs(i, source, target, crs_valid) || !crs_valid) {
			result_validity.SetInvalid(i);
			FlatVector::GetData<string_t>(result)[i] = string_t();
			continue;
		}

		std::string key = source;
		key.push_back('\x1f');
		key += target;
		auto it = cache.find(key);
		if (it == cache.end()) {
			it = cache.emplace(key, std::make_unique<CrsTransform>(source, target)).first;
		}

		auto &blob = geom_strings[geom_idx];
		auto payload =
		    ReprojectPayloadBlob(reinterpret_cast<const uint8_t *>(blob.GetData()), blob.GetSize(), *it->second);
		FlatVector::GetData<string_t>(result)[i] = StringVector::AddStringOrBlob(
		    result, string_t(reinterpret_cast<const char *>(payload.data()), payload.size()));
	}

	if (args.AllConstant()) {
		result.SetVectorType(VectorType::CONSTANT_VECTOR);
	}
}

// ST_3DTransform(geom, source_crs VARCHAR, target_crs VARCHAR)
static void ST_TransformStrFun(DataChunk &args, ExpressionState &state, Vector &result) {
	auto count = args.size();
	UnifiedVectorFormat src_data, tgt_data;
	args.data[1].ToUnifiedFormat(count, src_data);
	args.data[2].ToUnifiedFormat(count, tgt_data);
	auto src_strings = UnifiedVectorFormat::GetData<string_t>(src_data);
	auto tgt_strings = UnifiedVectorFormat::GetData<string_t>(tgt_data);

	TransformChunk(args, result, [&](idx_t i, std::string &source, std::string &target, bool &valid) -> bool {
		auto si = src_data.sel->get_index(i);
		auto ti = tgt_data.sel->get_index(i);
		if (!src_data.validity.RowIsValid(si) || !tgt_data.validity.RowIsValid(ti)) {
			valid = false;
			return false;
		}
		source = src_strings[si].GetString();
		target = tgt_strings[ti].GetString();
		return true;
	});
}

// ST_3DTransform(geom, source_srid INTEGER, target_srid INTEGER)
static void ST_TransformIntFun(DataChunk &args, ExpressionState &state, Vector &result) {
	using namespace duckdb_3d;
	auto count = args.size();
	UnifiedVectorFormat src_data, tgt_data;
	args.data[1].ToUnifiedFormat(count, src_data);
	args.data[2].ToUnifiedFormat(count, tgt_data);
	auto src_vals = UnifiedVectorFormat::GetData<int32_t>(src_data);
	auto tgt_vals = UnifiedVectorFormat::GetData<int32_t>(tgt_data);

	TransformChunk(args, result, [&](idx_t i, std::string &source, std::string &target, bool &valid) -> bool {
		auto si = src_data.sel->get_index(i);
		auto ti = tgt_data.sel->get_index(i);
		if (!src_data.validity.RowIsValid(si) || !tgt_data.validity.RowIsValid(ti)) {
			valid = false;
			return false;
		}
		source = EpsgToAuthString(src_vals[si]);
		target = EpsgToAuthString(tgt_vals[ti]);
		return true;
	});
}

// ──────────────────────────────────────────────────────────────
// Arrow-native ingestion (arrow-native-type branch): ST_3DFromArrowNative /
// ST_3DTryFromArrowNative consume the nested LIST<...<LIST<INTEGER>>>
// boundaries + LIST<STRUCT<x,y,z DOUBLE>> vertices columns cityparquet-rs /
// duckdb-cityjson write directly — no WKB bytes, no geometry_properties.
// ──────────────────────────────────────────────────────────────

//! boundaries: solid -> shell -> face -> ring -> index, 5 levels of LIST<INTEGER>.
LogicalType ArrowNativeGeometryType() {
	auto ring = LogicalType::LIST(LogicalType::INTEGER);
	auto face = LogicalType::LIST(ring);
	auto shell = LogicalType::LIST(face);
	auto solid = LogicalType::LIST(shell);
	return LogicalType::LIST(solid);
}

//! vertices: a flat pool, LIST<STRUCT<x,y,z DOUBLE>>, referenced by index
//! from the boundaries' innermost ring-index lists.
LogicalType ArrowNativeVerticesType() {
	child_list_t<LogicalType> fields;
	fields.push_back(make_pair("x", LogicalType::DOUBLE));
	fields.push_back(make_pair("y", LogicalType::DOUBLE));
	fields.push_back(make_pair("z", LogicalType::DOUBLE));
	return LogicalType::LIST(LogicalType::STRUCT(std::move(fields)));
}

//! A literal or constant-folded expression (as in a simple `SELECT ...` test
//! query, or Task 1's STRUCT overload before this fix) can produce a
//! non-FLAT_VECTOR at any nesting depth, not only the top level —
//! FlatVector::GetData/IsNull assert genuine flat vectors. `count` is this
//! level's own cardinality (its list's total element count across every row,
//! not the outer chunk's row count — list children are a single vector
//! shared/concatenated across all rows' entries).
//! Walks one row of the boundaries Vector, flattening each nested level as it
//! descends, into the kernel's plain-C++ CSR form (real per-level traversal,
//! not a raw buffer cast: DuckDB ListVectors are list_entry_t pairs into a
//! shared child, with possible non-flat intermediate children).
static duckdb_3d::ArrowNativeBoundaries ExtractArrowNativeBoundaries(Vector &boundaries_vec, idx_t row) {
	using namespace duckdb_3d;
	ArrowNativeBoundaries result;

	// CSR construction mirrors model_builder.cpp exactly: push a leading 0 for
	// each offset array once, then push the running cumulative count once
	// per completed element (shell/face/ring) — offsets[i] is element i's
	// start, offsets[i+1] its end, so pushing "the count so far" right after
	// finishing element i is exactly offsets[i+1].
	result.solid_shell_offsets.push_back(0);
	result.shell_face_offsets.push_back(0);
	result.face_ring_offsets.push_back(0);
	result.ring_vertex_offsets.push_back(0);

	uint32_t total_shells = 0, total_faces = 0, total_rings = 0;

	// Nullability invariant (design doc): "within a non-null geometry cell,
	// no nested list element is itself null — every solid/shell/face/ring
	// entry and every vertex index is present." Each level below checks the
	// CHILD vector's validity for the specific slot about to be dereferenced
	// before reading its list_entry_t/int32 — reading an unchecked null slot
	// is not merely "wrong data", it's genuinely undefined: two identical
	// queries were observed to disagree on whether the same null ring even
	// raised an error, because nothing constrains what bytes sit behind a
	// null slot.
	auto solid_entry = FlatVector::GetData<list_entry_t>(boundaries_vec)[row];
	auto &shell_vec = ListVector::GetEntry(boundaries_vec);
	FlattenIfNeeded(shell_vec, ListVector::GetListSize(boundaries_vec));
	auto &shell_validity = FlatVector::Validity(shell_vec);

	for (idx_t solid_idx = solid_entry.offset; solid_idx < solid_entry.offset + solid_entry.length; solid_idx++) {
		if (!shell_validity.RowIsValid(solid_idx)) {
			throw std::runtime_error("arrow-native geometry: null shell entry (no nested list element may be null)");
		}
		auto shell_entry = FlatVector::GetData<list_entry_t>(shell_vec)[solid_idx];
		auto &face_vec = ListVector::GetEntry(shell_vec);
		FlattenIfNeeded(face_vec, ListVector::GetListSize(shell_vec));
		auto &face_validity = FlatVector::Validity(face_vec);

		for (idx_t shell_idx = shell_entry.offset; shell_idx < shell_entry.offset + shell_entry.length; shell_idx++) {
			if (!face_validity.RowIsValid(shell_idx)) {
				throw std::runtime_error("arrow-native geometry: null face entry (no nested list element may be null)");
			}
			auto face_entry = FlatVector::GetData<list_entry_t>(face_vec)[shell_idx];
			auto &ring_vec = ListVector::GetEntry(face_vec);
			FlattenIfNeeded(ring_vec, ListVector::GetListSize(face_vec));
			auto &ring_validity = FlatVector::Validity(ring_vec);

			for (idx_t face_idx = face_entry.offset; face_idx < face_entry.offset + face_entry.length; face_idx++) {
				if (!ring_validity.RowIsValid(face_idx)) {
					throw std::runtime_error(
					    "arrow-native geometry: null ring entry (no nested list element may be null)");
				}
				auto ring_entry = FlatVector::GetData<list_entry_t>(ring_vec)[face_idx];
				auto &index_vec = ListVector::GetEntry(ring_vec);
				FlattenIfNeeded(index_vec, ListVector::GetListSize(ring_vec));
				auto &index_validity = FlatVector::Validity(index_vec);

				for (idx_t r = ring_entry.offset; r < ring_entry.offset + ring_entry.length; r++) {
					if (!index_validity.RowIsValid(r)) {
						throw std::runtime_error(
						    "arrow-native geometry: null vertex-index list entry (no nested list element may be null)");
					}
					auto idx_ring_entry = FlatVector::GetData<list_entry_t>(index_vec)[r];
					auto &leaf_vec = ListVector::GetEntry(index_vec);
					FlattenIfNeeded(leaf_vec, ListVector::GetListSize(index_vec));
					auto &leaf_validity = FlatVector::Validity(leaf_vec);
					auto leaf_data = FlatVector::GetData<int32_t>(leaf_vec);

					for (idx_t k = idx_ring_entry.offset; k < idx_ring_entry.offset + idx_ring_entry.length; k++) {
						if (!leaf_validity.RowIsValid(k)) {
							throw std::runtime_error(
							    "arrow-native geometry: null vertex-pool index (no nested list element may be null)");
						}
						int32_t raw = leaf_data[k];
						if (raw < 0) {
							throw std::runtime_error("arrow-native geometry: negative vertex-pool index");
						}
						result.ring_vertex_indices.push_back(static_cast<uint32_t>(raw));
					}
					total_rings++;
					result.ring_vertex_offsets.push_back(static_cast<uint32_t>(result.ring_vertex_indices.size()));
				}
				total_faces++;
				result.face_ring_offsets.push_back(total_rings);
			}
			total_shells++;
			result.shell_face_offsets.push_back(total_faces);
		}
		result.solid_shell_offsets.push_back(total_shells);
	}

	return result;
}

//! Walks one row of the vertices Vector into a plain vertex pool.
static std::vector<duckdb_3d::Vertex3D> ExtractArrowNativeVertices(Vector &vertices_vec, idx_t row) {
	using namespace duckdb_3d;
	auto vert_entry = FlatVector::GetData<list_entry_t>(vertices_vec)[row];
	auto &struct_vec = ListVector::GetEntry(vertices_vec);
	auto &children = StructVector::GetEntries(struct_vec);
	auto list_size = ListVector::GetListSize(vertices_vec);
	FlattenIfNeeded(*children[0], list_size);
	FlattenIfNeeded(*children[1], list_size);
	FlattenIfNeeded(*children[2], list_size);
	// Nullability invariant (design doc): "no Struct<x,y,z> entry is null and
	// none of x/y/z is null within a present entry."
	auto &struct_validity = FlatVector::Validity(struct_vec);
	auto &x_validity = FlatVector::Validity(*children[0]);
	auto &y_validity = FlatVector::Validity(*children[1]);
	auto &z_validity = FlatVector::Validity(*children[2]);
	auto x_data = FlatVector::GetData<double>(*children[0]);
	auto y_data = FlatVector::GetData<double>(*children[1]);
	auto z_data = FlatVector::GetData<double>(*children[2]);

	std::vector<Vertex3D> vertices;
	vertices.reserve(vert_entry.length);
	for (idx_t i = vert_entry.offset; i < vert_entry.offset + vert_entry.length; i++) {
		if (!struct_validity.RowIsValid(i) || !x_validity.RowIsValid(i) || !y_validity.RowIsValid(i) ||
		    !z_validity.RowIsValid(i)) {
			throw std::runtime_error("arrow-native geometry: null vertex-pool entry or coordinate "
			                         "(no vertex or coordinate may be null)");
		}
		vertices.push_back(Vertex3D {x_data[i], y_data[i], z_data[i]});
	}
	return vertices;
}

//! Design doc "critical invariant": the physical boundaries/vertices shape is
//! uniform across Solid-family and (padded) surface-family rows, so a single
//! column may legitimately mix them. Consumers MUST dispatch on
//! geometry_properties.type per row, never on physical shape — checking
//! shell-count/solid-count alone cannot distinguish a real single-shell
//! Solid from a padded MultiSurface (they are, by design, shape-identical).
static bool IsSolidFamilyType(const std::string &type) {
	return type == "Solid" || type == "MultiSolid" || type == "CompositeSolid";
}

static bool IsSurfaceFamilyType(const std::string &type) {
	return type == "MultiSurface" || type == "CompositeSurface";
}

//! Shared row logic for ST_3DFromArrowNative/ST_3DTryFromArrowNative, given an
//! already-extracted GeometryMetadata (from either the VARCHAR or STRUCT
//! geometry_properties overload). Checks the family, then builds+serializes.
//! metadata is also passed to the kernel so BuildSolidModelFromArrowNative
//! can regroup shells from metadata.shells — a real producer (confirmed:
//! cityparquet-rs's arrow_geom_write.rs) always pads a Solid's boundaries to
//! one physical shell, so trusting the physical nesting alone would merge
//! every interior shell into the exterior and silently skip
//! CheckInteriorShellWinding.
static std::string BuildSolidPayloadForRow(Vector &boundaries_vec, Vector &vertices_vec, idx_t row,
                                           const duckdb_3d::GeometryMetadata &metadata) {
	using namespace duckdb_3d;
	if (!IsSolidFamilyType(metadata.type)) {
		throw std::runtime_error("geometry_properties.type '" + metadata.type +
		                         "' is not a solid-family type (Solid/MultiSolid/CompositeSolid) — "
		                         "call ST_Geom3DFromArrowNative for surface types");
	}
	auto boundaries = ExtractArrowNativeBoundaries(boundaries_vec, row);
	auto vertices = ExtractArrowNativeVertices(vertices_vec, row);
	auto model = BuildSolidModelFromArrowNative(boundaries, vertices, metadata);
	auto payload = SerializePayload(model);
	return std::string(reinterpret_cast<const char *>(payload.data()), payload.size());
}

//! Shared row logic for ST_Geom3DFromArrowNative/ST_Geom3DTryFromArrowNative.
//! Surface types carry no shells (BuildGeomModelFromArrowNative's own
//! padding-dimension assertion is the only structural check needed), so no
//! metadata-driven regrouping applies here.
static std::string BuildGeomPayloadForRow(Vector &boundaries_vec, Vector &vertices_vec, idx_t row,
                                          const duckdb_3d::GeometryMetadata &metadata) {
	using namespace duckdb_3d;
	if (!IsSurfaceFamilyType(metadata.type)) {
		throw std::runtime_error("geometry_properties.type '" + metadata.type +
		                         "' is not a surface-family type (MultiSurface/CompositeSurface) — "
		                         "call ST_3DFromArrowNative for solid types");
	}
	auto boundaries = ExtractArrowNativeBoundaries(boundaries_vec, row);
	auto vertices = ExtractArrowNativeVertices(vertices_vec, row);
	auto model = BuildGeomModelFromArrowNative(boundaries, vertices);
	auto payload = SerializeGeomPayload(model);
	return std::string(reinterpret_cast<const char *>(payload.data()), payload.size());
}

// ──────────────────────────────────────────────────────────────
// ST_3DFromArrowNative(boundaries, vertices, geometry_properties VARCHAR) → SOLID_3D
// geometry_properties is JSON text, parsed via the same ParseGeometryProperties
// the WKB path uses — per the design doc, arrow-native keeps geometry_properties
// as VARCHAR-JSON on both paths precisely so this parsing step is identical,
// not something the new path gets to skip. `.type` is load-bearing (dispatch),
// not merely informational as on the WKB path.
// ──────────────────────────────────────────────────────────────
static void ST_3DFromArrowNativeFun(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &boundaries_vec = args.data[0];
	auto &vertices_vec = args.data[1];
	auto &meta_vec = args.data[2];
	auto count = args.size();
	bool all_constant = args.AllConstant();

	boundaries_vec.Flatten(count);
	vertices_vec.Flatten(count);
	auto &boundaries_validity = FlatVector::Validity(boundaries_vec);
	auto &vertices_validity = FlatVector::Validity(vertices_vec);
	auto &result_validity = FlatVector::Validity(result);

	UnifiedVectorFormat meta_data;
	meta_vec.ToUnifiedFormat(count, meta_data);
	auto meta_strings = UnifiedVectorFormat::GetData<string_t>(meta_data);

	for (idx_t i = 0; i < count; i++) {
		auto meta_idx = meta_data.sel->get_index(i);
		if (!boundaries_validity.RowIsValid(i) || !vertices_validity.RowIsValid(i) ||
		    !meta_data.validity.RowIsValid(meta_idx)) {
			result_validity.SetInvalid(i);
			FlatVector::GetData<string_t>(result)[i] = string_t();
			continue;
		}
		using namespace duckdb_3d;
		auto &meta_str = meta_strings[meta_idx];
		auto metadata = ParseGeometryProperties(std::string(meta_str.GetData(), meta_str.GetSize()));
		auto payload = BuildSolidPayloadForRow(boundaries_vec, vertices_vec, i, metadata);
		FlatVector::GetData<string_t>(result)[i] =
		    StringVector::AddStringOrBlob(result, string_t(payload.data(), payload.size()));
	}

	if (all_constant) {
		result.SetVectorType(VectorType::CONSTANT_VECTOR);
	}
}

// ──────────────────────────────────────────────────────────────
// ST_3DFromArrowNative(boundaries, vertices, geometry_properties STRUCT(...)) → SOLID_3D
// STRUCT overload, mirroring Task 1's ST_3DFromWKBWithStructMetaFun: binds
// directly against cityparquet-rs's real geometry_properties_lod* column
// (confirmed STRUCT, same type for WKB and arrow-native rows) without an
// explicit to_json() cast.
// ──────────────────────────────────────────────────────────────
static void ST_3DFromArrowNativeStructMetaFun(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &boundaries_vec = args.data[0];
	auto &vertices_vec = args.data[1];
	auto &meta_vec = args.data[2];
	auto count = args.size();
	bool all_constant = args.AllConstant();

	boundaries_vec.Flatten(count);
	vertices_vec.Flatten(count);
	meta_vec.Flatten(count);
	auto &boundaries_validity = FlatVector::Validity(boundaries_vec);
	auto &vertices_validity = FlatVector::Validity(vertices_vec);
	auto &meta_validity = FlatVector::Validity(meta_vec);
	auto &result_validity = FlatVector::Validity(result);

	for (idx_t i = 0; i < count; i++) {
		if (!boundaries_validity.RowIsValid(i) || !vertices_validity.RowIsValid(i) || !meta_validity.RowIsValid(i)) {
			result_validity.SetInvalid(i);
			FlatVector::GetData<string_t>(result)[i] = string_t();
			continue;
		}
		using namespace duckdb_3d;
		auto metadata = ExtractGeometryPropertiesFromStruct(meta_vec, i);
		auto payload = BuildSolidPayloadForRow(boundaries_vec, vertices_vec, i, metadata);
		FlatVector::GetData<string_t>(result)[i] =
		    StringVector::AddStringOrBlob(result, string_t(payload.data(), payload.size()));
	}

	if (all_constant) {
		result.SetVectorType(VectorType::CONSTANT_VECTOR);
	}
}

// ──────────────────────────────────────────────────────────────
// ST_3DTryFromArrowNative(boundaries, vertices, geometry_properties VARCHAR) → SOLID_3D or NULL
// ──────────────────────────────────────────────────────────────
static void ST_3DTryFromArrowNativeFun(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &boundaries_vec = args.data[0];
	auto &vertices_vec = args.data[1];
	auto &meta_vec = args.data[2];
	auto count = args.size();
	bool all_constant = args.AllConstant();

	boundaries_vec.Flatten(count);
	vertices_vec.Flatten(count);
	auto &boundaries_validity = FlatVector::Validity(boundaries_vec);
	auto &vertices_validity = FlatVector::Validity(vertices_vec);
	auto &result_validity = FlatVector::Validity(result);

	UnifiedVectorFormat meta_data;
	meta_vec.ToUnifiedFormat(count, meta_data);
	auto meta_strings = UnifiedVectorFormat::GetData<string_t>(meta_data);

	for (idx_t i = 0; i < count; i++) {
		auto meta_idx = meta_data.sel->get_index(i);
		if (!boundaries_validity.RowIsValid(i) || !vertices_validity.RowIsValid(i) ||
		    !meta_data.validity.RowIsValid(meta_idx)) {
			result_validity.SetInvalid(i);
			FlatVector::GetData<string_t>(result)[i] = string_t();
			continue;
		}
		try {
			using namespace duckdb_3d;
			auto &meta_str = meta_strings[meta_idx];
			auto metadata = ParseGeometryProperties(std::string(meta_str.GetData(), meta_str.GetSize()));
			auto payload = BuildSolidPayloadForRow(boundaries_vec, vertices_vec, i, metadata);
			FlatVector::GetData<string_t>(result)[i] =
			    StringVector::AddStringOrBlob(result, string_t(payload.data(), payload.size()));
		} catch (...) {
			result_validity.SetInvalid(i);
			FlatVector::GetData<string_t>(result)[i] = string_t();
		}
	}

	if (all_constant) {
		result.SetVectorType(VectorType::CONSTANT_VECTOR);
	}
}

// ──────────────────────────────────────────────────────────────
// ST_3DTryFromArrowNative(boundaries, vertices, geometry_properties STRUCT(...)) → SOLID_3D or NULL
// ──────────────────────────────────────────────────────────────
static void ST_3DTryFromArrowNativeStructMetaFun(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &boundaries_vec = args.data[0];
	auto &vertices_vec = args.data[1];
	auto &meta_vec = args.data[2];
	auto count = args.size();
	bool all_constant = args.AllConstant();

	boundaries_vec.Flatten(count);
	vertices_vec.Flatten(count);
	meta_vec.Flatten(count);
	auto &boundaries_validity = FlatVector::Validity(boundaries_vec);
	auto &vertices_validity = FlatVector::Validity(vertices_vec);
	auto &meta_validity = FlatVector::Validity(meta_vec);
	auto &result_validity = FlatVector::Validity(result);

	for (idx_t i = 0; i < count; i++) {
		if (!boundaries_validity.RowIsValid(i) || !vertices_validity.RowIsValid(i) || !meta_validity.RowIsValid(i)) {
			result_validity.SetInvalid(i);
			FlatVector::GetData<string_t>(result)[i] = string_t();
			continue;
		}
		try {
			using namespace duckdb_3d;
			auto metadata = ExtractGeometryPropertiesFromStruct(meta_vec, i);
			auto payload = BuildSolidPayloadForRow(boundaries_vec, vertices_vec, i, metadata);
			FlatVector::GetData<string_t>(result)[i] =
			    StringVector::AddStringOrBlob(result, string_t(payload.data(), payload.size()));
		} catch (...) {
			result_validity.SetInvalid(i);
			FlatVector::GetData<string_t>(result)[i] = string_t();
		}
	}

	if (all_constant) {
		result.SetVectorType(VectorType::CONSTANT_VECTOR);
	}
}

// ──────────────────────────────────────────────────────────────
// ST_Geom3DFromArrowNative(boundaries, vertices, geometry_properties VARCHAR) → GEOM_3D
// (boundaries padded to solid-count 1 / shell-count 1 for surface-family
// types — see BuildGeomModelFromArrowNative. That padding-shape assertion is
// a defensive secondary check; the primary dispatch is geometry_properties.type.)
// ──────────────────────────────────────────────────────────────
static void ST_Geom3DFromArrowNativeFun(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &boundaries_vec = args.data[0];
	auto &vertices_vec = args.data[1];
	auto &meta_vec = args.data[2];
	auto count = args.size();
	bool all_constant = args.AllConstant();

	boundaries_vec.Flatten(count);
	vertices_vec.Flatten(count);
	auto &boundaries_validity = FlatVector::Validity(boundaries_vec);
	auto &vertices_validity = FlatVector::Validity(vertices_vec);
	auto &result_validity = FlatVector::Validity(result);

	UnifiedVectorFormat meta_data;
	meta_vec.ToUnifiedFormat(count, meta_data);
	auto meta_strings = UnifiedVectorFormat::GetData<string_t>(meta_data);

	for (idx_t i = 0; i < count; i++) {
		auto meta_idx = meta_data.sel->get_index(i);
		if (!boundaries_validity.RowIsValid(i) || !vertices_validity.RowIsValid(i) ||
		    !meta_data.validity.RowIsValid(meta_idx)) {
			result_validity.SetInvalid(i);
			FlatVector::GetData<string_t>(result)[i] = string_t();
			continue;
		}
		using namespace duckdb_3d;
		auto &meta_str = meta_strings[meta_idx];
		auto metadata = ParseGeometryProperties(std::string(meta_str.GetData(), meta_str.GetSize()));
		auto payload = BuildGeomPayloadForRow(boundaries_vec, vertices_vec, i, metadata);
		FlatVector::GetData<string_t>(result)[i] =
		    StringVector::AddStringOrBlob(result, string_t(payload.data(), payload.size()));
	}

	if (all_constant) {
		result.SetVectorType(VectorType::CONSTANT_VECTOR);
	}
}

// ──────────────────────────────────────────────────────────────
// ST_Geom3DFromArrowNative(boundaries, vertices, geometry_properties STRUCT(...)) → GEOM_3D
// ──────────────────────────────────────────────────────────────
static void ST_Geom3DFromArrowNativeStructMetaFun(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &boundaries_vec = args.data[0];
	auto &vertices_vec = args.data[1];
	auto &meta_vec = args.data[2];
	auto count = args.size();
	bool all_constant = args.AllConstant();

	boundaries_vec.Flatten(count);
	vertices_vec.Flatten(count);
	meta_vec.Flatten(count);
	auto &boundaries_validity = FlatVector::Validity(boundaries_vec);
	auto &vertices_validity = FlatVector::Validity(vertices_vec);
	auto &meta_validity = FlatVector::Validity(meta_vec);
	auto &result_validity = FlatVector::Validity(result);

	for (idx_t i = 0; i < count; i++) {
		if (!boundaries_validity.RowIsValid(i) || !vertices_validity.RowIsValid(i) || !meta_validity.RowIsValid(i)) {
			result_validity.SetInvalid(i);
			FlatVector::GetData<string_t>(result)[i] = string_t();
			continue;
		}
		using namespace duckdb_3d;
		auto metadata = ExtractGeometryPropertiesFromStruct(meta_vec, i);
		auto payload = BuildGeomPayloadForRow(boundaries_vec, vertices_vec, i, metadata);
		FlatVector::GetData<string_t>(result)[i] =
		    StringVector::AddStringOrBlob(result, string_t(payload.data(), payload.size()));
	}

	if (all_constant) {
		result.SetVectorType(VectorType::CONSTANT_VECTOR);
	}
}

// ──────────────────────────────────────────────────────────────
// ST_Geom3DTryFromArrowNative(boundaries, vertices, geometry_properties VARCHAR) → GEOM_3D or NULL
// ──────────────────────────────────────────────────────────────
static void ST_Geom3DTryFromArrowNativeFun(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &boundaries_vec = args.data[0];
	auto &vertices_vec = args.data[1];
	auto &meta_vec = args.data[2];
	auto count = args.size();
	bool all_constant = args.AllConstant();

	boundaries_vec.Flatten(count);
	vertices_vec.Flatten(count);
	auto &boundaries_validity = FlatVector::Validity(boundaries_vec);
	auto &vertices_validity = FlatVector::Validity(vertices_vec);
	auto &result_validity = FlatVector::Validity(result);

	UnifiedVectorFormat meta_data;
	meta_vec.ToUnifiedFormat(count, meta_data);
	auto meta_strings = UnifiedVectorFormat::GetData<string_t>(meta_data);

	for (idx_t i = 0; i < count; i++) {
		auto meta_idx = meta_data.sel->get_index(i);
		if (!boundaries_validity.RowIsValid(i) || !vertices_validity.RowIsValid(i) ||
		    !meta_data.validity.RowIsValid(meta_idx)) {
			result_validity.SetInvalid(i);
			FlatVector::GetData<string_t>(result)[i] = string_t();
			continue;
		}
		try {
			using namespace duckdb_3d;
			auto &meta_str = meta_strings[meta_idx];
			auto metadata = ParseGeometryProperties(std::string(meta_str.GetData(), meta_str.GetSize()));
			auto payload = BuildGeomPayloadForRow(boundaries_vec, vertices_vec, i, metadata);
			FlatVector::GetData<string_t>(result)[i] =
			    StringVector::AddStringOrBlob(result, string_t(payload.data(), payload.size()));
		} catch (...) {
			result_validity.SetInvalid(i);
			FlatVector::GetData<string_t>(result)[i] = string_t();
		}
	}

	if (all_constant) {
		result.SetVectorType(VectorType::CONSTANT_VECTOR);
	}
}

// ──────────────────────────────────────────────────────────────
// ST_Geom3DTryFromArrowNative(boundaries, vertices, geometry_properties STRUCT(...)) → GEOM_3D or NULL
// ──────────────────────────────────────────────────────────────
static void ST_Geom3DTryFromArrowNativeStructMetaFun(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &boundaries_vec = args.data[0];
	auto &vertices_vec = args.data[1];
	auto &meta_vec = args.data[2];
	auto count = args.size();
	bool all_constant = args.AllConstant();

	boundaries_vec.Flatten(count);
	vertices_vec.Flatten(count);
	meta_vec.Flatten(count);
	auto &boundaries_validity = FlatVector::Validity(boundaries_vec);
	auto &vertices_validity = FlatVector::Validity(vertices_vec);
	auto &meta_validity = FlatVector::Validity(meta_vec);
	auto &result_validity = FlatVector::Validity(result);

	for (idx_t i = 0; i < count; i++) {
		if (!boundaries_validity.RowIsValid(i) || !vertices_validity.RowIsValid(i) || !meta_validity.RowIsValid(i)) {
			result_validity.SetInvalid(i);
			FlatVector::GetData<string_t>(result)[i] = string_t();
			continue;
		}
		try {
			using namespace duckdb_3d;
			auto metadata = ExtractGeometryPropertiesFromStruct(meta_vec, i);
			auto payload = BuildGeomPayloadForRow(boundaries_vec, vertices_vec, i, metadata);
			FlatVector::GetData<string_t>(result)[i] =
			    StringVector::AddStringOrBlob(result, string_t(payload.data(), payload.size()));
		} catch (...) {
			result_validity.SetInvalid(i);
			FlatVector::GetData<string_t>(result)[i] = string_t();
		}
	}

	if (all_constant) {
		result.SetVectorType(VectorType::CONSTANT_VECTOR);
	}
}

// ──────────────────────────────────────────────────────────────
// Extension registration
// ──────────────────────────────────────────────────────────────
static void LoadInternal(ExtensionLoader &loader) {
	// Register SOLID_3D type (alias over BLOB)
	auto solid_3d_type = LogicalType(LogicalTypeId::BLOB);
	solid_3d_type.SetAlias("SOLID_3D");
	loader.RegisterType("SOLID_3D", solid_3d_type);

	// Register GEOM_3D type: a general 3D geometry (point/line/polygon/multi/
	// polyhedral-surface), also an alias over BLOB with its own payload (§16.2).
	auto geom_3d_type = LogicalType(LogicalTypeId::BLOB);
	geom_3d_type.SetAlias("GEOM_3D");
	loader.RegisterType("GEOM_3D", geom_3d_type);

	// geometry_properties STRUCT("type" VARCHAR, surfaces JSON, face_semantics
	// INTEGER[], shells INTEGER[][]) — the shape duckdb-cityjson's
	// arrow-native-type branch (commit d334b26) and cityparquet-rs both emit
	// for geometry_properties_lod* instead of/alongside VARCHAR JSON text.
	// Declared here (before any registration that needs it) so both the
	// GEOM_3D and SOLID_3D arrow-native/WKB STRUCT overloads below can share it.
	child_list_t<LogicalType> geom_props_fields;
	geom_props_fields.push_back(make_pair("type", LogicalType::VARCHAR));
	geom_props_fields.push_back(make_pair("surfaces", LogicalType::VARCHAR));
	geom_props_fields.push_back(make_pair("face_semantics", LogicalType::LIST(LogicalType::INTEGER)));
	geom_props_fields.push_back(make_pair("shells", LogicalType::LIST(LogicalType::LIST(LogicalType::INTEGER))));
	auto geometry_properties_struct_type = LogicalType::STRUCT(std::move(geom_props_fields));

	// Test helper: generate tetrahedron WKB
	loader.RegisterFunction(
	    ScalarFunction("st_aswkbpolyhedraltetra", {}, LogicalType::BLOB, ST_AsWKBPolyhedralTetraFun));
	loader.RegisterFunction(ScalarFunction("st_aswkbopentetra", {}, LogicalType::BLOB, ST_AsWKBOpenTetraFun));
	loader.RegisterFunction(ScalarFunction("st_aswkbhollowcube", {}, LogicalType::BLOB, ST_AsWKBHollowCubeFun));
	loader.RegisterFunction(ScalarFunction("st_aswkbmulticube", {}, LogicalType::BLOB, ST_AsWKBMultiCubeFun));
	loader.RegisterFunction(ScalarFunction("st_aswkbpointz",
	                                       {LogicalType::DOUBLE, LogicalType::DOUBLE, LogicalType::DOUBLE},
	                                       LogicalType::BLOB, ST_AsWKBPointZFun));
	loader.RegisterFunction(ScalarFunction("st_aswkblinez", {}, LogicalType::BLOB, ST_AsWKBLineZFun));
	loader.RegisterFunction(ScalarFunction("st_aswkbmultilinez", {}, LogicalType::BLOB, ST_AsWKBMultiLineZFun));
	loader.RegisterFunction(ScalarFunction("st_aswkbpolygonz", {}, LogicalType::BLOB, ST_AsWKBPolygonZFun));
	loader.RegisterFunction(ScalarFunction("st_aswkbwarpedpolygonz", {}, LogicalType::BLOB, ST_AsWKBWarpedPolygonZFun));
	loader.RegisterFunction(ScalarFunction("st_aswkbmultipointz", {}, LogicalType::BLOB, ST_AsWKBMultiPointZFun));
	loader.RegisterFunction(ScalarFunction("st_aswkbmultipolygonz", {}, LogicalType::BLOB, ST_AsWKBMultiPolygonZFun));

	// GEOM_3D construction and accessors
	loader.RegisterFunction(ScalarFunction("st_geom3dfromwkb", {LogicalType::BLOB}, geom_3d_type, ST_Geom3DFromWKBFun));

	// ST_Geom3DFromArrowNative / ST_Geom3DTryFromArrowNative: VARCHAR and STRUCT
	// geometry_properties overloads (STRUCT binds directly against
	// cityparquet-rs's real geometry_properties_lod* column).
	ScalarFunctionSet geom3d_from_arrow_native_set("st_geom3dfromarrownative");
	auto geom3d_from_arrow_native =
	    ScalarFunction({ArrowNativeGeometryType(), ArrowNativeVerticesType(), LogicalType::VARCHAR}, geom_3d_type,
	                   ST_Geom3DFromArrowNativeFun);
	geom3d_from_arrow_native.SetNullHandling(FunctionNullHandling::SPECIAL_HANDLING);
	geom3d_from_arrow_native_set.AddFunction(geom3d_from_arrow_native);
	auto geom3d_from_arrow_native_struct =
	    ScalarFunction({ArrowNativeGeometryType(), ArrowNativeVerticesType(), geometry_properties_struct_type},
	                   geom_3d_type, ST_Geom3DFromArrowNativeStructMetaFun);
	geom3d_from_arrow_native_struct.SetNullHandling(FunctionNullHandling::SPECIAL_HANDLING);
	geom3d_from_arrow_native_set.AddFunction(geom3d_from_arrow_native_struct);
	loader.RegisterFunction(geom3d_from_arrow_native_set);

	ScalarFunctionSet geom3d_try_from_arrow_native_set("st_geom3dtryfromarrownative");
	auto geom3d_try_from_arrow_native =
	    ScalarFunction({ArrowNativeGeometryType(), ArrowNativeVerticesType(), LogicalType::VARCHAR}, geom_3d_type,
	                   ST_Geom3DTryFromArrowNativeFun);
	geom3d_try_from_arrow_native.SetNullHandling(FunctionNullHandling::SPECIAL_HANDLING);
	geom3d_try_from_arrow_native_set.AddFunction(geom3d_try_from_arrow_native);
	auto geom3d_try_from_arrow_native_struct =
	    ScalarFunction({ArrowNativeGeometryType(), ArrowNativeVerticesType(), geometry_properties_struct_type},
	                   geom_3d_type, ST_Geom3DTryFromArrowNativeStructMetaFun);
	geom3d_try_from_arrow_native_struct.SetNullHandling(FunctionNullHandling::SPECIAL_HANDLING);
	geom3d_try_from_arrow_native_set.AddFunction(geom3d_try_from_arrow_native_struct);
	loader.RegisterFunction(geom3d_try_from_arrow_native_set);

	loader.RegisterFunction(
	    ScalarFunction("st_3dgeometrytype", {geom_3d_type}, LogicalType::VARCHAR, ST_GeometryTypeFun));
	loader.RegisterFunction(ScalarFunction("st_3dx", {geom_3d_type}, LogicalType::DOUBLE, ST_XFun));
	loader.RegisterFunction(ScalarFunction("st_3dy", {geom_3d_type}, LogicalType::DOUBLE, ST_YFun));
	loader.RegisterFunction(ScalarFunction("st_3dz", {geom_3d_type}, LogicalType::DOUBLE, ST_ZFun));
	loader.RegisterFunction(ScalarFunction("st_coorddim", {geom_3d_type}, LogicalType::INTEGER, ST_CoordDimFun));
	loader.RegisterFunction(ScalarFunction("st_3ddimension", {geom_3d_type}, LogicalType::INTEGER, ST_DimensionFun));
	loader.RegisterFunction(
	    ScalarFunction("st_3dnumgeometries", {geom_3d_type}, LogicalType::INTEGER, ST_NumGeometriesFun));
	loader.RegisterFunction(ScalarFunction("st_3dlength", {geom_3d_type}, LogicalType::DOUBLE, ST_3DLengthFun));
	loader.RegisterFunction(
	    ScalarFunction("st_3ddistance", {geom_3d_type, geom_3d_type}, LogicalType::DOUBLE, ST_3DDistanceFun));
	loader.RegisterFunction(ScalarFunction("st_3ddwithin", {geom_3d_type, geom_3d_type, LogicalType::DOUBLE},
	                                       LogicalType::BOOLEAN, ST_3DDWithinFun));
	loader.RegisterFunction(
	    ScalarFunction("st_3dmaxdistance", {geom_3d_type, geom_3d_type}, LogicalType::DOUBLE, ST_3DMaxDistanceFun));
	loader.RegisterFunction(ScalarFunction("st_3ddfullywithin", {geom_3d_type, geom_3d_type, LogicalType::DOUBLE},
	                                       LogicalType::BOOLEAN, ST_3DDFullyWithinFun));
	loader.RegisterFunction(
	    ScalarFunction("st_3dintersects", {geom_3d_type, geom_3d_type}, LogicalType::BOOLEAN, ST_3DIntersectsFun));
	loader.RegisterFunction(
	    ScalarFunction("st_3dclosestpoint", {geom_3d_type, geom_3d_type}, geom_3d_type, ST_3DClosestPointFun));
	loader.RegisterFunction(
	    ScalarFunction("st_3dshortestline", {geom_3d_type, geom_3d_type}, geom_3d_type, ST_3DShortestLineFun));
	loader.RegisterFunction(ScalarFunction("st_3dastext", {geom_3d_type}, LogicalType::VARCHAR, ST_AsTextFun));
	loader.RegisterFunction(ScalarFunction("st_3dasgeojson", {geom_3d_type}, LogicalType::VARCHAR, ST_AsGeoJSONFun));
	loader.RegisterFunction(ScalarFunction("st_3dasbinary", {geom_3d_type}, LogicalType::BLOB, ST_AsBinaryFun));
	loader.RegisterFunction(ScalarFunction("st_isplanar", {geom_3d_type}, LogicalType::BOOLEAN, ST_IsPlanarFun));
	loader.RegisterFunction(ScalarFunction("st_3dcentroid", {geom_3d_type}, geom_3d_type, ST_3DCentroidFun));
	loader.RegisterFunction(ScalarFunction("st_force3d", {geom_3d_type}, geom_3d_type, ST_Force3DFun));
	loader.RegisterFunction(ScalarFunction("st_3dconvexhull", {geom_3d_type}, geom_3d_type, ST_ConvexHullFun));
	// Returns plain BLOB (like st_3dfromwkb) so the SOLID_3D measurement/introspection
	// functions, which bind on BLOB, compose directly on the result.
	loader.RegisterFunction(
	    ScalarFunction("st_3dextrude", {geom_3d_type, LogicalType::DOUBLE}, LogicalType::BLOB, ST_3DExtrudeFun));
	loader.RegisterFunction(ScalarFunction("st_makesolid", {geom_3d_type}, LogicalType::BLOB, ST_MakeSolidFun));

	// ST_3DFromWKB: 1-arg, 2-arg (VARCHAR), and 2-arg (STRUCT) overloads
	ScalarFunctionSet from_wkb_set("st_3dfromwkb");
	from_wkb_set.AddFunction(ScalarFunction({LogicalType::BLOB}, LogicalType::BLOB, ST_3DFromWKBFun));
	auto from_wkb_2arg =
	    ScalarFunction({LogicalType::BLOB, LogicalType::VARCHAR}, LogicalType::BLOB, ST_3DFromWKBWithMetaFun);
	from_wkb_2arg.SetNullHandling(FunctionNullHandling::SPECIAL_HANDLING);
	from_wkb_set.AddFunction(from_wkb_2arg);
	// (BLOB, ANY): accept CityParquet's geometry_properties STRUCT directly; the
	// bind routes VARCHAR/JSON/SQLNULL back to the JSON executor.
	auto from_wkb_any = ScalarFunction({LogicalType::BLOB, LogicalType::ANY}, LogicalType::BLOB,
	                                   ST_3DFromWKBWithStructFun, FromWkbAnyBind);
	from_wkb_any.SetNullHandling(FunctionNullHandling::SPECIAL_HANDLING);
	from_wkb_set.AddFunction(from_wkb_any);
	loader.RegisterFunction(from_wkb_set);

	// ST_3DTryFromWKB: 1-arg, 2-arg (VARCHAR), and 2-arg (STRUCT) overloads
	ScalarFunctionSet try_from_wkb_set("st_3dtryfromwkb");
	try_from_wkb_set.AddFunction(ScalarFunction({LogicalType::BLOB}, LogicalType::BLOB, ST_3DTryFromWKBFun));
	auto try_from_wkb_2arg =
	    ScalarFunction({LogicalType::BLOB, LogicalType::VARCHAR}, LogicalType::BLOB, ST_3DTryFromWKBWithMetaFun);
	try_from_wkb_2arg.SetNullHandling(FunctionNullHandling::SPECIAL_HANDLING);
	try_from_wkb_set.AddFunction(try_from_wkb_2arg);
	auto try_from_wkb_any = ScalarFunction({LogicalType::BLOB, LogicalType::ANY}, LogicalType::BLOB,
	                                       ST_3DTryFromWKBWithStructFun, TryFromWkbAnyBind);
	try_from_wkb_any.SetNullHandling(FunctionNullHandling::SPECIAL_HANDLING);
	try_from_wkb_set.AddFunction(try_from_wkb_any);
	loader.RegisterFunction(try_from_wkb_set);

	// ST_3DFromArrowNative / ST_3DTryFromArrowNative(boundaries, vertices, geometry_properties) -> SOLID_3D
	// VARCHAR and STRUCT geometry_properties overloads, mirroring the WKB set above.
	ScalarFunctionSet from_arrow_native_set("st_3dfromarrownative");
	auto from_arrow_native =
	    ScalarFunction({ArrowNativeGeometryType(), ArrowNativeVerticesType(), LogicalType::VARCHAR}, LogicalType::BLOB,
	                   ST_3DFromArrowNativeFun);
	from_arrow_native.SetNullHandling(FunctionNullHandling::SPECIAL_HANDLING);
	from_arrow_native_set.AddFunction(from_arrow_native);
	auto from_arrow_native_struct =
	    ScalarFunction({ArrowNativeGeometryType(), ArrowNativeVerticesType(), geometry_properties_struct_type},
	                   LogicalType::BLOB, ST_3DFromArrowNativeStructMetaFun);
	from_arrow_native_struct.SetNullHandling(FunctionNullHandling::SPECIAL_HANDLING);
	from_arrow_native_set.AddFunction(from_arrow_native_struct);
	loader.RegisterFunction(from_arrow_native_set);

	ScalarFunctionSet try_from_arrow_native_set("st_3dtryfromarrownative");
	auto try_from_arrow_native =
	    ScalarFunction({ArrowNativeGeometryType(), ArrowNativeVerticesType(), LogicalType::VARCHAR}, LogicalType::BLOB,
	                   ST_3DTryFromArrowNativeFun);
	try_from_arrow_native.SetNullHandling(FunctionNullHandling::SPECIAL_HANDLING);
	try_from_arrow_native_set.AddFunction(try_from_arrow_native);
	auto try_from_arrow_native_struct =
	    ScalarFunction({ArrowNativeGeometryType(), ArrowNativeVerticesType(), geometry_properties_struct_type},
	                   LogicalType::BLOB, ST_3DTryFromArrowNativeStructMetaFun);
	try_from_arrow_native_struct.SetNullHandling(FunctionNullHandling::SPECIAL_HANDLING);
	try_from_arrow_native_set.AddFunction(try_from_arrow_native_struct);
	loader.RegisterFunction(try_from_arrow_native_set);

	// ST_3DAsWKB(solid SOLID_3D) -> BLOB
	loader.RegisterFunction(ScalarFunction("st_3daswkb", {LogicalType::BLOB}, LogicalType::BLOB, ST_3DAsWKBFun));

	// Introspection: counts
	loader.RegisterFunction(
	    ScalarFunction("st_3dnumsolids", {LogicalType::BLOB}, LogicalType::BIGINT, ST_3DNumSolidsFun));
	loader.RegisterFunction(
	    ScalarFunction("st_3dnumshells", {LogicalType::BLOB}, LogicalType::BIGINT, ST_3DNumShellsFun));
	loader.RegisterFunction(
	    ScalarFunction("st_3dnumfaces", {LogicalType::BLOB}, LogicalType::BIGINT, ST_3DNumFacesFun));

	// Introspection: bounds
	child_list_t<LogicalType> bbox_children;
	bbox_children.push_back({"min_x", LogicalType::DOUBLE});
	bbox_children.push_back({"min_y", LogicalType::DOUBLE});
	bbox_children.push_back({"min_z", LogicalType::DOUBLE});
	bbox_children.push_back({"max_x", LogicalType::DOUBLE});
	bbox_children.push_back({"max_y", LogicalType::DOUBLE});
	bbox_children.push_back({"max_z", LogicalType::DOUBLE});
	auto bbox_type = LogicalType::STRUCT(std::move(bbox_children));
	loader.RegisterFunction(ScalarFunction("st_3dbounds", {LogicalType::BLOB}, bbox_type, ST_3DBoundsFun));

	// Validation functions
	loader.RegisterFunction(
	    ScalarFunction("st_3disclosed", {LogicalType::BLOB}, LogicalType::BOOLEAN, ST_3DIsClosedFun));
	loader.RegisterFunction(
	    ScalarFunction("st_3dismanifold", {LogicalType::BLOB}, LogicalType::BOOLEAN, ST_3DIsManifoldFun));
	loader.RegisterFunction(
	    ScalarFunction("st_3disoriented", {LogicalType::BLOB}, LogicalType::BOOLEAN, ST_3DIsOrientedFun));

	// Validation report
	child_list_t<LogicalType> report_children;
	report_children.push_back({"is_valid", LogicalType::BOOLEAN});
	report_children.push_back({"is_closed", LogicalType::BOOLEAN});
	report_children.push_back({"is_manifold", LogicalType::BOOLEAN});
	report_children.push_back({"is_oriented", LogicalType::BOOLEAN});
	report_children.push_back({"solid_count", LogicalType::BIGINT});
	report_children.push_back({"shell_count", LogicalType::BIGINT});
	report_children.push_back({"face_count", LogicalType::BIGINT});
	report_children.push_back({"open_edge_count", LogicalType::BIGINT});
	report_children.push_back({"non_manifold_edge_count", LogicalType::BIGINT});
	report_children.push_back({"degenerate_face_count", LogicalType::BIGINT});
	report_children.push_back({"orientation_error_count", LogicalType::BIGINT});
	report_children.push_back({"code", LogicalType::VARCHAR});
	report_children.push_back({"message", LogicalType::VARCHAR});
	auto report_type = LogicalType::STRUCT(std::move(report_children));
	loader.RegisterFunction(
	    ScalarFunction("st_3dvalidationreport", {LogicalType::BLOB}, report_type, ST_3DValidationReportFun));

	// Measurement functions
	loader.RegisterFunction(
	    ScalarFunction("st_3dsurfacearea", {LogicalType::BLOB}, LogicalType::DOUBLE, ST_3DSurfaceAreaFun));
	// ST_3DArea is the surface-area measurement under a PostGIS-aligned name.
	loader.RegisterFunction(ScalarFunction("st_3darea", {LogicalType::BLOB}, LogicalType::DOUBLE, ST_3DSurfaceAreaFun));
	loader.RegisterFunction(ScalarFunction("st_3dvolume", {LogicalType::BLOB}, LogicalType::DOUBLE, ST_3DVolumeFun));
	// ST_3DFootprintArea dispatches by payload magic, so accept SOLID_3D and GEOM_3D (and
	// raw BLOB) — same pattern as ST_3DZMin/ST_3DZMax.
	ScalarFunctionSet area_set("st_3dfootprintarea");
	area_set.AddFunction(ScalarFunction({LogicalType::BLOB}, LogicalType::DOUBLE, ST_AreaFun));
	area_set.AddFunction(ScalarFunction({solid_3d_type}, LogicalType::DOUBLE, ST_AreaFun));
	area_set.AddFunction(ScalarFunction({geom_3d_type}, LogicalType::DOUBLE, ST_AreaFun));
	loader.RegisterFunction(area_set);
	loader.RegisterFunction(
	    ScalarFunction("st_3dperimeter", {LogicalType::BLOB}, LogicalType::DOUBLE, ST_3DPerimeterFun));

	// Accessor functions
	ScalarFunctionSet ndims_set("st_ndims");
	ndims_set.AddFunction(ScalarFunction({LogicalType::BLOB}, LogicalType::INTEGER, ST_NDimsFun));
	ndims_set.AddFunction(ScalarFunction({solid_3d_type}, LogicalType::INTEGER, ST_NDimsFun));
	ndims_set.AddFunction(ScalarFunction({geom_3d_type}, LogicalType::INTEGER, ST_NDimsFun));
	loader.RegisterFunction(ndims_set);

	ScalarFunctionSet hasz_set("st_3dhasz");
	hasz_set.AddFunction(ScalarFunction({LogicalType::BLOB}, LogicalType::BOOLEAN, ST_HasZFun));
	hasz_set.AddFunction(ScalarFunction({solid_3d_type}, LogicalType::BOOLEAN, ST_HasZFun));
	hasz_set.AddFunction(ScalarFunction({geom_3d_type}, LogicalType::BOOLEAN, ST_HasZFun));
	loader.RegisterFunction(hasz_set);

	// ST_3DZMin / ST_3DZMax: class-generic bbox accessors, accept SOLID_3D, GEOM_3D,
	// and plain BLOB values. Multiple overloads are needed because DuckDB treats
	// named type aliases as distinct for function resolution.
	ScalarFunctionSet zmin_set("st_3dzmin");
	zmin_set.AddFunction(ScalarFunction({LogicalType::BLOB}, LogicalType::DOUBLE, ST_ZMinFun));
	zmin_set.AddFunction(ScalarFunction({solid_3d_type}, LogicalType::DOUBLE, ST_ZMinFun));
	zmin_set.AddFunction(ScalarFunction({geom_3d_type}, LogicalType::DOUBLE, ST_ZMinFun));
	loader.RegisterFunction(zmin_set);

	ScalarFunctionSet zmax_set("st_3dzmax");
	zmax_set.AddFunction(ScalarFunction({LogicalType::BLOB}, LogicalType::DOUBLE, ST_ZMaxFun));
	zmax_set.AddFunction(ScalarFunction({solid_3d_type}, LogicalType::DOUBLE, ST_ZMaxFun));
	zmax_set.AddFunction(ScalarFunction({geom_3d_type}, LogicalType::DOUBLE, ST_ZMaxFun));
	loader.RegisterFunction(zmax_set);

	// Transform functions
	ScalarFunctionSet translate_set("st_3dtranslate");
	translate_set.AddFunction(
	    ScalarFunction({LogicalType::BLOB, LogicalType::DOUBLE, LogicalType::DOUBLE, LogicalType::DOUBLE},
	                   LogicalType::BLOB, ST_TranslateFun));
	translate_set.AddFunction(
	    ScalarFunction({solid_3d_type, LogicalType::DOUBLE, LogicalType::DOUBLE, LogicalType::DOUBLE}, solid_3d_type,
	                   ST_TranslateFun));
	translate_set.AddFunction(
	    ScalarFunction({geom_3d_type, LogicalType::DOUBLE, LogicalType::DOUBLE, LogicalType::DOUBLE}, geom_3d_type,
	                   ST_TranslateGeomFun));
	loader.RegisterFunction(translate_set);
	ScalarFunctionSet scale_set("st_3dscale");
	scale_set.AddFunction(
	    ScalarFunction({LogicalType::BLOB, LogicalType::DOUBLE, LogicalType::DOUBLE, LogicalType::DOUBLE},
	                   LogicalType::BLOB, ST_ScaleFun));
	scale_set.AddFunction(ScalarFunction({solid_3d_type, LogicalType::DOUBLE, LogicalType::DOUBLE, LogicalType::DOUBLE},
	                                     solid_3d_type, ST_ScaleFun));
	scale_set.AddFunction(ScalarFunction({geom_3d_type, LogicalType::DOUBLE, LogicalType::DOUBLE, LogicalType::DOUBLE},
	                                     geom_3d_type, ST_ScaleGeomFun));
	loader.RegisterFunction(scale_set);
	ScalarFunctionSet rotatex_set("st_3drotatex");
	rotatex_set.AddFunction(ScalarFunction({LogicalType::BLOB, LogicalType::DOUBLE}, LogicalType::BLOB, ST_RotateXFun));
	rotatex_set.AddFunction(ScalarFunction({solid_3d_type, LogicalType::DOUBLE}, solid_3d_type, ST_RotateXFun));
	rotatex_set.AddFunction(ScalarFunction({geom_3d_type, LogicalType::DOUBLE}, geom_3d_type, ST_RotateXGeomFun));
	loader.RegisterFunction(rotatex_set);

	ScalarFunctionSet rotatey_set("st_3drotatey");
	rotatey_set.AddFunction(ScalarFunction({LogicalType::BLOB, LogicalType::DOUBLE}, LogicalType::BLOB, ST_RotateYFun));
	rotatey_set.AddFunction(ScalarFunction({solid_3d_type, LogicalType::DOUBLE}, solid_3d_type, ST_RotateYFun));
	rotatey_set.AddFunction(ScalarFunction({geom_3d_type, LogicalType::DOUBLE}, geom_3d_type, ST_RotateYGeomFun));
	loader.RegisterFunction(rotatey_set);

	ScalarFunctionSet rotatez_set("st_3drotatez");
	rotatez_set.AddFunction(ScalarFunction({LogicalType::BLOB, LogicalType::DOUBLE}, LogicalType::BLOB, ST_RotateZFun));
	rotatez_set.AddFunction(ScalarFunction({solid_3d_type, LogicalType::DOUBLE}, solid_3d_type, ST_RotateZFun));
	rotatez_set.AddFunction(ScalarFunction({geom_3d_type, LogicalType::DOUBLE}, geom_3d_type, ST_RotateZGeomFun));
	loader.RegisterFunction(rotatez_set);

	// ST_3DTransform: 2D CRS reprojection. EPSG-integer and CRS-string forms, each
	// on SOLID_3D and GEOM_3D. Output type equals input type.
	ScalarFunctionSet transform_set("st_3dtransform");
	transform_set.AddFunction(ScalarFunction({LogicalType::BLOB, LogicalType::INTEGER, LogicalType::INTEGER},
	                                         LogicalType::BLOB, ST_TransformIntFun));
	transform_set.AddFunction(ScalarFunction({LogicalType::BLOB, LogicalType::VARCHAR, LogicalType::VARCHAR},
	                                         LogicalType::BLOB, ST_TransformStrFun));
	transform_set.AddFunction(
	    ScalarFunction({solid_3d_type, LogicalType::INTEGER, LogicalType::INTEGER}, solid_3d_type, ST_TransformIntFun));
	transform_set.AddFunction(
	    ScalarFunction({geom_3d_type, LogicalType::INTEGER, LogicalType::INTEGER}, geom_3d_type, ST_TransformIntFun));
	transform_set.AddFunction(
	    ScalarFunction({solid_3d_type, LogicalType::VARCHAR, LogicalType::VARCHAR}, solid_3d_type, ST_TransformStrFun));
	transform_set.AddFunction(
	    ScalarFunction({geom_3d_type, LogicalType::VARCHAR, LogicalType::VARCHAR}, geom_3d_type, ST_TransformStrFun));
	loader.RegisterFunction(transform_set);
}

void ThreeDExtension::Load(ExtensionLoader &loader) {
	LoadInternal(loader);
}

std::string ThreeDExtension::Name() {
	return "three_d";
}

std::string ThreeDExtension::Version() const {
#ifdef EXT_VERSION_THREE_D
	return EXT_VERSION_THREE_D;
#else
	return "";
#endif
}

} // namespace duckdb

extern "C" {

DUCKDB_CPP_EXTENSION_ENTRY(three_d, loader) {
	duckdb::LoadInternal(loader);
}
}
