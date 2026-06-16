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
#include "kernel/metadata_parser.hpp"
#include "kernel/geom_model.hpp"
#include "kernel/geom_wkb_parser.hpp"
#include "kernel/geom_payload.hpp"
#include "duckdb/function/function_set.hpp"

#include <cmath>
#include <cstring>

namespace duckdb {

// ──────────────────────────────────────────────────────────────
// Helper: generate a test tetrahedron WKB (for SQL tests)
// ──────────────────────────────────────────────────────────────
static std::vector<uint8_t> BuildTetrahedronWKB() {
	std::vector<uint8_t> buf;
	auto u8 = [&](uint8_t v) { buf.push_back(v); };
	auto u32 = [&](uint32_t v) {
		buf.push_back(v & 0xFF); buf.push_back((v >> 8) & 0xFF);
		buf.push_back((v >> 16) & 0xFF); buf.push_back((v >> 24) & 0xFF);
	};
	auto f64 = [&](double v) {
		uint8_t b[8]; memcpy(b, &v, 8);
		buf.insert(buf.end(), b, b + 8);
	};
	auto ring = [&](double x0, double y0, double z0, double x1, double y1, double z1,
	                double x2, double y2, double z2) {
		u32(4);
		f64(x0); f64(y0); f64(z0);
		f64(x1); f64(y1); f64(z1);
		f64(x2); f64(y2); f64(z2);
		f64(x0); f64(y0); f64(z0);
	};
	auto poly_header = [&](uint32_t num_rings) {
		u8(1); u32(1003); u32(num_rings); // byte-order, PolygonZ, num_rings
	};

	u8(1); u32(1015); u32(4);
	poly_header(1); ring(0,0,0, 0,1,0, 1,0,0);
	poly_header(1); ring(0,0,0, 1,0,0, 0,0,1);
	poly_header(1); ring(1,0,0, 0,1,0, 0,0,1);
	poly_header(1); ring(0,0,0, 0,0,1, 0,1,0);
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
	auto u8 = [&](uint8_t v) { buf.push_back(v); };
	auto u32 = [&](uint32_t v) {
		buf.push_back(v & 0xFF); buf.push_back((v >> 8) & 0xFF);
		buf.push_back((v >> 16) & 0xFF); buf.push_back((v >> 24) & 0xFF);
	};
	auto f64 = [&](double v) {
		uint8_t b[8]; memcpy(b, &v, 8);
		buf.insert(buf.end(), b, b + 8);
	};
	auto ring = [&](double x0, double y0, double z0, double x1, double y1, double z1,
	                double x2, double y2, double z2) {
		u32(4);
		f64(x0); f64(y0); f64(z0);
		f64(x1); f64(y1); f64(z1);
		f64(x2); f64(y2); f64(z2);
		f64(x0); f64(y0); f64(z0);
	};
	auto poly_header = [&](uint32_t num_rings) {
		u8(1); u32(1003); u32(num_rings); // byte-order, PolygonZ, num_rings
	};

	u8(1); u32(1015); u32(3);
	poly_header(1); ring(0,0,0, 1,0,0, 0,0,1);
	poly_header(1); ring(1,0,0, 0,1,0, 0,0,1);
	poly_header(1); ring(0,0,0, 0,0,1, 0,1,0);
	return buf;
}

static void ST_AsWKBOpenTetraFun(DataChunk &args, ExpressionState &state, Vector &result) {
	auto wkb = BuildOpenTetrahedronWKB();
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
		return StringVector::AddStringOrBlob(result, string_t(reinterpret_cast<const char *>(payload.data()), payload.size()));
	});
}

// ──────────────────────────────────────────────────────────────
// ST_3DTryFromWKB(wkb BLOB) → SOLID_3D (BLOB) or NULL
// ──────────────────────────────────────────────────────────────
static void ST_3DTryFromWKBFun(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &wkb_vec = args.data[0];
	UnaryExecutor::ExecuteWithNulls<string_t, string_t>(wkb_vec, result, args.size(),
		[&](string_t wkb, ValidityMask &mask, idx_t idx) -> string_t {
			using namespace duckdb_3d;
			try {
				auto surfaces = ParseWKB(reinterpret_cast<const uint8_t *>(wkb.GetData()), wkb.GetSize());
				auto model = BuildSolidModel(surfaces);
				auto payload = SerializePayload(model);
				return StringVector::AddStringOrBlob(result, string_t(reinterpret_cast<const char *>(payload.data()), payload.size()));
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
static void ST_3DNumSolidsFun(DataChunk &args, ExpressionState &state, Vector &result) {
	UnaryExecutor::Execute<string_t, int64_t>(args.data[0], result, args.size(), [](string_t solid) {
		using namespace duckdb_3d;
		auto model = DeserializePayload(reinterpret_cast<const uint8_t *>(solid.GetData()), solid.GetSize());
		return static_cast<int64_t>(model.SolidCount());
	});
}

static void ST_3DNumShellsFun(DataChunk &args, ExpressionState &state, Vector &result) {
	UnaryExecutor::Execute<string_t, int64_t>(args.data[0], result, args.size(), [](string_t solid) {
		using namespace duckdb_3d;
		auto model = DeserializePayload(reinterpret_cast<const uint8_t *>(solid.GetData()), solid.GetSize());
		return static_cast<int64_t>(model.ShellCount());
	});
}

static void ST_3DNumFacesFun(DataChunk &args, ExpressionState &state, Vector &result) {
	UnaryExecutor::Execute<string_t, int64_t>(args.data[0], result, args.size(), [](string_t solid) {
		using namespace duckdb_3d;
		auto model = DeserializePayload(reinterpret_cast<const uint8_t *>(solid.GetData()), solid.GetSize());
		return static_cast<int64_t>(model.FaceCount());
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
		auto model = DeserializePayload(reinterpret_cast<const uint8_t *>(blob.GetData()), blob.GetSize());

		FlatVector::GetData<double>(min_x_vec)[i] = model.bbox.min_x;
		FlatVector::GetData<double>(min_y_vec)[i] = model.bbox.min_y;
		FlatVector::GetData<double>(min_z_vec)[i] = model.bbox.min_z;
		FlatVector::GetData<double>(max_x_vec)[i] = model.bbox.max_x;
		FlatVector::GetData<double>(max_y_vec)[i] = model.bbox.max_y;
		FlatVector::GetData<double>(max_z_vec)[i] = model.bbox.max_z;
	}
}

// ──────────────────────────────────────────────────────────────
// Validation: ST_3DIsClosed, ST_3DIsManifold, ST_3DIsOriented
// ──────────────────────────────────────────────────────────────
static void ST_3DIsClosedFun(DataChunk &args, ExpressionState &state, Vector &result) {
	UnaryExecutor::Execute<string_t, bool>(args.data[0], result, args.size(), [](string_t solid) {
		using namespace duckdb_3d;
		auto model = DeserializePayload(reinterpret_cast<const uint8_t *>(solid.GetData()), solid.GetSize());
		return model.validation.is_closed;
	});
}

static void ST_3DIsManifoldFun(DataChunk &args, ExpressionState &state, Vector &result) {
	UnaryExecutor::Execute<string_t, bool>(args.data[0], result, args.size(), [](string_t solid) {
		using namespace duckdb_3d;
		auto model = DeserializePayload(reinterpret_cast<const uint8_t *>(solid.GetData()), solid.GetSize());
		return model.validation.is_manifold;
	});
}

static void ST_3DIsOrientedFun(DataChunk &args, ExpressionState &state, Vector &result) {
	UnaryExecutor::Execute<string_t, bool>(args.data[0], result, args.size(), [](string_t solid) {
		using namespace duckdb_3d;
		auto model = DeserializePayload(reinterpret_cast<const uint8_t *>(solid.GetData()), solid.GetSize());
		return model.validation.is_oriented;
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
		auto model = DeserializePayload(reinterpret_cast<const uint8_t *>(blob.GetData()), blob.GetSize());
		auto &vc = model.validation;

		FlatVector::GetData<bool>(is_valid_vec)[i] = vc.is_valid;
		FlatVector::GetData<bool>(is_closed_vec)[i] = vc.is_closed;
		FlatVector::GetData<bool>(is_manifold_vec)[i] = vc.is_manifold;
		FlatVector::GetData<bool>(is_oriented_vec)[i] = vc.is_oriented;
		FlatVector::GetData<int64_t>(solid_count_vec)[i] = model.SolidCount();
		FlatVector::GetData<int64_t>(shell_count_vec)[i] = model.ShellCount();
		FlatVector::GetData<int64_t>(face_count_vec)[i] = model.FaceCount();
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
			if (!vc.is_closed) issues.push_back("not closed");
			if (!vc.is_manifold) issues.push_back("non-manifold edges");
			if (!vc.is_oriented) issues.push_back("orientation inconsistent");
			if (vc.degenerate_face_count > 0) issues.push_back("degenerate faces");
			code_str = "INVALID";
			msg_str = "Invalid solid: ";
			for (size_t j = 0; j < issues.size(); j++) {
				if (j > 0) msg_str += ", ";
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

static void ST_AreaFun(DataChunk &args, ExpressionState &state, Vector &result) {
	UnaryExecutor::Execute<string_t, double>(args.data[0], result, args.size(), [](string_t solid) {
		using namespace duckdb_3d;
		auto model = DeserializePayload(reinterpret_cast<const uint8_t *>(solid.GetData()), solid.GetSize());
		return ComputeFootprintArea(model);
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
	UnaryExecutor::Execute<string_t, int32_t>(args.data[0], result, args.size(), [](string_t solid) {
		return CoordinateDimension3D();
	});
}

static void ST_CoordDimFun(DataChunk &args, ExpressionState &state, Vector &result) {
	UnaryExecutor::Execute<string_t, int32_t>(args.data[0], result, args.size(), [](string_t geom) {
		return CoordinateDimension3D();
	});
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
		case PayloadKind::Solid: {
			auto model = DeserializePayload(data, size);
			return model.bbox.min_z;
		}
		case PayloadKind::Geom: {
			auto model = DeserializeGeomPayload(data, size);
			return model.bbox.min_z;
		}
		default:
			throw InvalidInputException("ST_ZMin: argument is not a SOLID_3D or GEOM_3D value");
		}
	});
}

static void ST_ZMaxFun(DataChunk &args, ExpressionState &state, Vector &result) {
	UnaryExecutor::Execute<string_t, double>(args.data[0], result, args.size(), [](string_t blob) {
		using namespace duckdb_3d;
		auto data = reinterpret_cast<const uint8_t *>(blob.GetData());
		auto size = blob.GetSize();
		switch (GetPayloadKind(data, size)) {
		case PayloadKind::Solid: {
			auto model = DeserializePayload(data, size);
			return model.bbox.max_z;
		}
		case PayloadKind::Geom: {
			auto model = DeserializeGeomPayload(data, size);
			return model.bbox.max_z;
		}
		default:
			throw InvalidInputException("ST_ZMax: argument is not a SOLID_3D or GEOM_3D value");
		}
	});
}

// ──────────────────────────────────────────────────────────────
// Transforms: ST_Translate(solid SOLID_3D, dx, dy, dz DOUBLE) → SOLID_3D
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
		model.bbox.min_x += dx; model.bbox.max_x += dx;
		model.bbox.min_y += dy; model.bbox.max_y += dy;
		model.bbox.min_z += dz; model.bbox.max_z += dz;

		auto payload = SerializePayload(model);
		FlatVector::GetData<string_t>(result)[i] = StringVector::AddStringOrBlob(
		    result, string_t(reinterpret_cast<const char *>(payload.data()), payload.size()));
	}

	if (args.AllConstant()) {
		result.SetVectorType(VectorType::CONSTANT_VECTOR);
	}
}

// ──────────────────────────────────────────────────────────────
// Transforms: ST_Translate(geom GEOM_3D, dx, dy, dz DOUBLE) → GEOM_3D
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
		model.bbox.min_x += dx; model.bbox.max_x += dx;
		model.bbox.min_y += dy; model.bbox.max_y += dy;
		model.bbox.min_z += dz; model.bbox.max_z += dz;

		auto payload = SerializeGeomPayload(model);
		FlatVector::GetData<string_t>(result)[i] = StringVector::AddStringOrBlob(
		    result, string_t(reinterpret_cast<const char *>(payload.data()), payload.size()));
	}

	if (args.AllConstant()) {
		result.SetVectorType(VectorType::CONSTANT_VECTOR);
	}
}

// ──────────────────────────────────────────────────────────────
// Transforms: ST_Scale(solid SOLID_3D, sx, sy, sz DOUBLE) → SOLID_3D
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
		// Scale about the origin. Topology and winding consistency are preserved
		// for non-degenerate factors, so validation flags carry over; only the
		// bbox is recomputed (factors may be negative and swap min/max).
		for (auto &v : model.vertices) {
			v.x *= sx;
			v.y *= sy;
			v.z *= sz;
		}
		model.ComputeBBox();

		auto payload = SerializePayload(model);
		FlatVector::GetData<string_t>(result)[i] = StringVector::AddStringOrBlob(
		    result, string_t(reinterpret_cast<const char *>(payload.data()), payload.size()));
	}

	if (args.AllConstant()) {
		result.SetVectorType(VectorType::CONSTANT_VECTOR);
	}
}

// ──────────────────────────────────────────────────────────────
// Transforms: ST_RotateX / ST_RotateY / ST_RotateZ(solid, radians) → SOLID_3D
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
	return StringVector::AddStringOrBlob(
	    result, string_t(reinterpret_cast<const char *>(payload.data()), payload.size()));
}

static void ST_RotateXFun(DataChunk &args, ExpressionState &state, Vector &result) {
	BinaryExecutor::Execute<string_t, double, string_t>(args.data[0], args.data[1], result, args.size(),
		[&](string_t solid, double radians) { return RotateSolidBlob(result, solid, radians, RotationAxis::X); });
}

static void ST_RotateYFun(DataChunk &args, ExpressionState &state, Vector &result) {
	BinaryExecutor::Execute<string_t, double, string_t>(args.data[0], args.data[1], result, args.size(),
		[&](string_t solid, double radians) { return RotateSolidBlob(result, solid, radians, RotationAxis::Y); });
}

static void ST_RotateZFun(DataChunk &args, ExpressionState &state, Vector &result) {
	BinaryExecutor::Execute<string_t, double, string_t>(args.data[0], args.data[1], result, args.size(),
		[&](string_t solid, double radians) { return RotateSolidBlob(result, solid, radians, RotationAxis::Z); });
}

// ──────────────────────────────────────────────────────────────
// GEOM_3D: general geometry construction and accessors
// ──────────────────────────────────────────────────────────────

// Test helper: build a Point Z WKB from x, y, z.
static void ST_AsWKBPointZFun(DataChunk &args, ExpressionState &state, Vector &result) {
	TernaryExecutor::Execute<double, double, double, string_t>(
	    args.data[0], args.data[1], args.data[2], result, args.size(),
	    [&](double x, double y, double z) {
		    std::vector<uint8_t> buf;
		    buf.push_back(1); // little-endian
		    uint32_t type = 1001; // Point Z
		    buf.push_back(type & 0xFF); buf.push_back((type >> 8) & 0xFF);
		    buf.push_back((type >> 16) & 0xFF); buf.push_back((type >> 24) & 0xFF);
		    auto push_f64 = [&](double v) {
			    uint8_t b[8]; memcpy(b, &v, 8); buf.insert(buf.end(), b, b + 8);
		    };
		    push_f64(x); push_f64(y); push_f64(z);
		    return StringVector::AddStringOrBlob(
		        result, string_t(reinterpret_cast<const char *>(buf.data()), buf.size()));
	    });
}

// Test helper: build a LineString Z WKB from (0,0,0) to (3,4,12).
static void ST_AsWKBLineZFun(DataChunk &args, ExpressionState &state, Vector &result) {
	std::vector<uint8_t> buf;
	auto push_u32 = [&](uint32_t v) {
		buf.push_back(v & 0xFF); buf.push_back((v >> 8) & 0xFF);
		buf.push_back((v >> 16) & 0xFF); buf.push_back((v >> 24) & 0xFF);
	};
	auto push_f64 = [&](double v) {
		uint8_t b[8]; memcpy(b, &v, 8); buf.insert(buf.end(), b, b + 8);
	};
	buf.push_back(1); // little-endian
	push_u32(1002); // LineString Z
	push_u32(2);
	push_f64(0.0); push_f64(0.0); push_f64(0.0);
	push_f64(3.0); push_f64(4.0); push_f64(12.0);

	result.SetVectorType(VectorType::CONSTANT_VECTOR);
	ConstantVector::GetData<string_t>(result)[0] = StringVector::AddStringOrBlob(
	    result, string_t(reinterpret_cast<const char *>(buf.data()), buf.size()));
}

// Test helper: build a MultiLineString Z with lengths 13 and 5.
static void ST_AsWKBMultiLineZFun(DataChunk &args, ExpressionState &state, Vector &result) {
	std::vector<uint8_t> buf;
	auto push_u32 = [&](uint32_t v) {
		buf.push_back(v & 0xFF); buf.push_back((v >> 8) & 0xFF);
		buf.push_back((v >> 16) & 0xFF); buf.push_back((v >> 24) & 0xFF);
	};
	auto push_f64 = [&](double v) {
		uint8_t b[8]; memcpy(b, &v, 8); buf.insert(buf.end(), b, b + 8);
	};
	auto push_line = [&](double x0, double y0, double z0, double x1, double y1, double z1) {
		buf.push_back(1); // little-endian child
		push_u32(1002); // LineString Z
		push_u32(2);
		push_f64(x0); push_f64(y0); push_f64(z0);
		push_f64(x1); push_f64(y1); push_f64(z1);
	};

	buf.push_back(1); // little-endian
	push_u32(1005); // MultiLineString Z
	push_u32(2);
	push_line(0.0, 0.0, 0.0, 3.0, 4.0, 12.0);
	push_line(10.0, 10.0, 10.0, 13.0, 14.0, 10.0);

	result.SetVectorType(VectorType::CONSTANT_VECTOR);
	ConstantVector::GetData<string_t>(result)[0] = StringVector::AddStringOrBlob(
	    result, string_t(reinterpret_cast<const char *>(buf.data()), buf.size()));
}

// ST_Geom3DFromWKB(wkb BLOB) → GEOM_3D
// (named to avoid clashing with DuckDB core's st_geomfromwkb -> GEOMETRY)
static void ST_Geom3DFromWKBFun(DataChunk &args, ExpressionState &state, Vector &result) {
	UnaryExecutor::Execute<string_t, string_t>(args.data[0], result, args.size(), [&](string_t wkb) {
		using namespace duckdb_3d;
		auto model = ParseGeomWKB(reinterpret_cast<const uint8_t *>(wkb.GetData()), wkb.GetSize());
		auto payload = SerializeGeomPayload(model);
		return StringVector::AddStringOrBlob(
		    result, string_t(reinterpret_cast<const char *>(payload.data()), payload.size()));
	});
}

static const char *GeomTypeName(duckdb_3d::GeomType type) {
	using namespace duckdb_3d;
	switch (type) {
	case GeomType::Point: return "ST_Point";
	case GeomType::LineString: return "ST_LineString";
	case GeomType::Polygon: return "ST_Polygon";
	case GeomType::MultiPoint: return "ST_MultiPoint";
	case GeomType::MultiLineString: return "ST_MultiLineString";
	case GeomType::MultiPolygon: return "ST_MultiPolygon";
	case GeomType::GeometryCollection: return "ST_GeometryCollection";
	case GeomType::PolyhedralSurface: return "ST_PolyhedralSurface";
	default: return "ST_Geometry";
	}
}

// ST_GeometryType(geom GEOM_3D) → VARCHAR
static void ST_GeometryTypeFun(DataChunk &args, ExpressionState &state, Vector &result) {
	UnaryExecutor::Execute<string_t, string_t>(args.data[0], result, args.size(), [&](string_t geom) {
		using namespace duckdb_3d;
		auto model = DeserializeGeomPayload(reinterpret_cast<const uint8_t *>(geom.GetData()), geom.GetSize());
		return StringVector::AddString(result, GeomTypeName(model.type));
	});
}

// Point ordinate accessors: ST_X / ST_Y / ST_Z(point GEOM_3D) → DOUBLE.
enum class Ordinate { X, Y, Z };

static double PointOrdinate(string_t geom, Ordinate ord) {
	using namespace duckdb_3d;
	auto model = DeserializeGeomPayload(reinterpret_cast<const uint8_t *>(geom.GetData()), geom.GetSize());
	if (model.type != GeomType::Point) {
		throw InvalidInputException("ST_X/ST_Y/ST_Z: argument is not a Point");
	}
	if (model.vertices.empty()) {
		throw InvalidInputException("ST_X/ST_Y/ST_Z: empty point");
	}
	const auto &v = model.vertices[0];
	switch (ord) {
	case Ordinate::X: return v.x;
	case Ordinate::Y: return v.y;
	default: return v.z;
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

// ST_Dimension(geom GEOM_3D) → INTEGER
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

	// Test helper: generate tetrahedron WKB
	loader.RegisterFunction(ScalarFunction("st_aswkbpolyhedraltetra", {}, LogicalType::BLOB, ST_AsWKBPolyhedralTetraFun));
	loader.RegisterFunction(ScalarFunction("st_aswkbopentetra", {}, LogicalType::BLOB, ST_AsWKBOpenTetraFun));
	loader.RegisterFunction(ScalarFunction("st_aswkbpointz",
	    {LogicalType::DOUBLE, LogicalType::DOUBLE, LogicalType::DOUBLE}, LogicalType::BLOB, ST_AsWKBPointZFun));
	loader.RegisterFunction(ScalarFunction("st_aswkblinez", {}, LogicalType::BLOB, ST_AsWKBLineZFun));
	loader.RegisterFunction(ScalarFunction("st_aswkbmultilinez", {}, LogicalType::BLOB, ST_AsWKBMultiLineZFun));

	// GEOM_3D construction and accessors
	loader.RegisterFunction(ScalarFunction("st_geom3dfromwkb", {LogicalType::BLOB}, geom_3d_type, ST_Geom3DFromWKBFun));
	loader.RegisterFunction(ScalarFunction("st_geometrytype", {geom_3d_type}, LogicalType::VARCHAR, ST_GeometryTypeFun));
	loader.RegisterFunction(ScalarFunction("st_x", {geom_3d_type}, LogicalType::DOUBLE, ST_XFun));
	loader.RegisterFunction(ScalarFunction("st_y", {geom_3d_type}, LogicalType::DOUBLE, ST_YFun));
	loader.RegisterFunction(ScalarFunction("st_z", {geom_3d_type}, LogicalType::DOUBLE, ST_ZFun));
	loader.RegisterFunction(ScalarFunction("st_coorddim", {geom_3d_type}, LogicalType::INTEGER, ST_CoordDimFun));
	loader.RegisterFunction(ScalarFunction("st_dimension", {geom_3d_type}, LogicalType::INTEGER, ST_DimensionFun));
	loader.RegisterFunction(ScalarFunction("st_3dlength", {geom_3d_type}, LogicalType::DOUBLE, ST_3DLengthFun));

	// ST_3DFromWKB: 1-arg and 2-arg overloads
	ScalarFunctionSet from_wkb_set("st_3dfromwkb");
	from_wkb_set.AddFunction(ScalarFunction({LogicalType::BLOB}, LogicalType::BLOB, ST_3DFromWKBFun));
	auto from_wkb_2arg = ScalarFunction({LogicalType::BLOB, LogicalType::VARCHAR}, LogicalType::BLOB, ST_3DFromWKBWithMetaFun);
	from_wkb_2arg.SetNullHandling(FunctionNullHandling::SPECIAL_HANDLING);
	from_wkb_set.AddFunction(from_wkb_2arg);
	loader.RegisterFunction(from_wkb_set);

	// ST_3DTryFromWKB: 1-arg and 2-arg overloads
	ScalarFunctionSet try_from_wkb_set("st_3dtryfromwkb");
	try_from_wkb_set.AddFunction(ScalarFunction({LogicalType::BLOB}, LogicalType::BLOB, ST_3DTryFromWKBFun));
	auto try_from_wkb_2arg = ScalarFunction({LogicalType::BLOB, LogicalType::VARCHAR}, LogicalType::BLOB, ST_3DTryFromWKBWithMetaFun);
	try_from_wkb_2arg.SetNullHandling(FunctionNullHandling::SPECIAL_HANDLING);
	try_from_wkb_set.AddFunction(try_from_wkb_2arg);
	loader.RegisterFunction(try_from_wkb_set);

	// ST_3DAsWKB(solid SOLID_3D) -> BLOB
	loader.RegisterFunction(ScalarFunction("st_3daswkb", {LogicalType::BLOB}, LogicalType::BLOB, ST_3DAsWKBFun));

	// Introspection: counts
	loader.RegisterFunction(ScalarFunction("st_3dnumsolids", {LogicalType::BLOB}, LogicalType::BIGINT, ST_3DNumSolidsFun));
	loader.RegisterFunction(ScalarFunction("st_3dnumshells", {LogicalType::BLOB}, LogicalType::BIGINT, ST_3DNumShellsFun));
	loader.RegisterFunction(ScalarFunction("st_3dnumfaces", {LogicalType::BLOB}, LogicalType::BIGINT, ST_3DNumFacesFun));

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
	loader.RegisterFunction(ScalarFunction("st_3disclosed", {LogicalType::BLOB}, LogicalType::BOOLEAN, ST_3DIsClosedFun));
	loader.RegisterFunction(ScalarFunction("st_3dismanifold", {LogicalType::BLOB}, LogicalType::BOOLEAN, ST_3DIsManifoldFun));
	loader.RegisterFunction(ScalarFunction("st_3disoriented", {LogicalType::BLOB}, LogicalType::BOOLEAN, ST_3DIsOrientedFun));

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
	loader.RegisterFunction(ScalarFunction("st_3dvalidationreport", {LogicalType::BLOB}, report_type, ST_3DValidationReportFun));

	// Measurement functions
	loader.RegisterFunction(ScalarFunction("st_3dsurfacearea", {LogicalType::BLOB}, LogicalType::DOUBLE, ST_3DSurfaceAreaFun));
	// ST_3DArea is the surface-area measurement under a PostGIS-aligned name.
	loader.RegisterFunction(ScalarFunction("st_3darea", {LogicalType::BLOB}, LogicalType::DOUBLE, ST_3DSurfaceAreaFun));
	loader.RegisterFunction(ScalarFunction("st_3dvolume", {LogicalType::BLOB}, LogicalType::DOUBLE, ST_3DVolumeFun));
	loader.RegisterFunction(ScalarFunction("st_area", {LogicalType::BLOB}, LogicalType::DOUBLE, ST_AreaFun));
	loader.RegisterFunction(ScalarFunction("st_3dperimeter", {LogicalType::BLOB}, LogicalType::DOUBLE, ST_3DPerimeterFun));

	// Accessor functions
	ScalarFunctionSet ndims_set("st_ndims");
	ndims_set.AddFunction(ScalarFunction({LogicalType::BLOB}, LogicalType::INTEGER, ST_NDimsFun));
	ndims_set.AddFunction(ScalarFunction({solid_3d_type}, LogicalType::INTEGER, ST_NDimsFun));
	ndims_set.AddFunction(ScalarFunction({geom_3d_type}, LogicalType::INTEGER, ST_NDimsFun));
	loader.RegisterFunction(ndims_set);

	ScalarFunctionSet hasz_set("st_hasz");
	hasz_set.AddFunction(ScalarFunction({LogicalType::BLOB}, LogicalType::BOOLEAN, ST_HasZFun));
	hasz_set.AddFunction(ScalarFunction({solid_3d_type}, LogicalType::BOOLEAN, ST_HasZFun));
	hasz_set.AddFunction(ScalarFunction({geom_3d_type}, LogicalType::BOOLEAN, ST_HasZFun));
	loader.RegisterFunction(hasz_set);

	// ST_ZMin / ST_ZMax: class-generic bbox accessors, accept SOLID_3D, GEOM_3D,
	// and plain BLOB values. Multiple overloads are needed because DuckDB treats
	// named type aliases as distinct for function resolution.
	ScalarFunctionSet zmin_set("st_zmin");
	zmin_set.AddFunction(ScalarFunction({LogicalType::BLOB}, LogicalType::DOUBLE, ST_ZMinFun));
	zmin_set.AddFunction(ScalarFunction({solid_3d_type}, LogicalType::DOUBLE, ST_ZMinFun));
	zmin_set.AddFunction(ScalarFunction({geom_3d_type}, LogicalType::DOUBLE, ST_ZMinFun));
	loader.RegisterFunction(zmin_set);

	ScalarFunctionSet zmax_set("st_zmax");
	zmax_set.AddFunction(ScalarFunction({LogicalType::BLOB}, LogicalType::DOUBLE, ST_ZMaxFun));
	zmax_set.AddFunction(ScalarFunction({solid_3d_type}, LogicalType::DOUBLE, ST_ZMaxFun));
	zmax_set.AddFunction(ScalarFunction({geom_3d_type}, LogicalType::DOUBLE, ST_ZMaxFun));
	loader.RegisterFunction(zmax_set);

	// Transform functions
	ScalarFunctionSet translate_set("st_translate");
	translate_set.AddFunction(ScalarFunction(
	    {LogicalType::BLOB, LogicalType::DOUBLE, LogicalType::DOUBLE, LogicalType::DOUBLE},
	    LogicalType::BLOB, ST_TranslateFun));
	translate_set.AddFunction(ScalarFunction(
	    {solid_3d_type, LogicalType::DOUBLE, LogicalType::DOUBLE, LogicalType::DOUBLE},
	    solid_3d_type, ST_TranslateFun));
	translate_set.AddFunction(ScalarFunction(
	    {geom_3d_type, LogicalType::DOUBLE, LogicalType::DOUBLE, LogicalType::DOUBLE},
	    geom_3d_type, ST_TranslateGeomFun));
	loader.RegisterFunction(translate_set);
	loader.RegisterFunction(ScalarFunction("st_scale",
	    {LogicalType::BLOB, LogicalType::DOUBLE, LogicalType::DOUBLE, LogicalType::DOUBLE},
	    LogicalType::BLOB, ST_ScaleFun));
	loader.RegisterFunction(ScalarFunction("st_rotatex", {LogicalType::BLOB, LogicalType::DOUBLE},
	    LogicalType::BLOB, ST_RotateXFun));
	loader.RegisterFunction(ScalarFunction("st_rotatey", {LogicalType::BLOB, LogicalType::DOUBLE},
	    LogicalType::BLOB, ST_RotateYFun));
	loader.RegisterFunction(ScalarFunction("st_rotatez", {LogicalType::BLOB, LogicalType::DOUBLE},
	    LogicalType::BLOB, ST_RotateZFun));
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
