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
#include "duckdb/function/function_set.hpp"

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

// ──────────────────────────────────────────────────────────────
// Accessors: ST_NDims
// ──────────────────────────────────────────────────────────────
static void ST_NDimsFun(DataChunk &args, ExpressionState &state, Vector &result) {
	UnaryExecutor::Execute<string_t, int32_t>(args.data[0], result, args.size(), [](string_t solid) {
		// v1 stores transformed XYZ coordinates only: coordinate dimension is always 3.
		return static_cast<int32_t>(3);
	});
}

static void ST_HasZFun(DataChunk &args, ExpressionState &state, Vector &result) {
	UnaryExecutor::Execute<string_t, bool>(args.data[0], result, args.size(), [](string_t solid) {
		// v1 geometries always carry a Z ordinate.
		return true;
	});
}

static void ST_ZMinFun(DataChunk &args, ExpressionState &state, Vector &result) {
	UnaryExecutor::Execute<string_t, double>(args.data[0], result, args.size(), [](string_t solid) {
		using namespace duckdb_3d;
		auto model = DeserializePayload(reinterpret_cast<const uint8_t *>(solid.GetData()), solid.GetSize());
		return model.bbox.min_z;
	});
}

static void ST_ZMaxFun(DataChunk &args, ExpressionState &state, Vector &result) {
	UnaryExecutor::Execute<string_t, double>(args.data[0], result, args.size(), [](string_t solid) {
		using namespace duckdb_3d;
		auto model = DeserializePayload(reinterpret_cast<const uint8_t *>(solid.GetData()), solid.GetSize());
		return model.bbox.max_z;
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
// Extension registration
// ──────────────────────────────────────────────────────────────
static void LoadInternal(ExtensionLoader &loader) {
	// Register SOLID_3D type (alias over BLOB)
	auto solid_3d_type = LogicalType(LogicalTypeId::BLOB);
	solid_3d_type.SetAlias("SOLID_3D");
	loader.RegisterType("SOLID_3D", solid_3d_type);

	// Test helper: generate tetrahedron WKB
	loader.RegisterFunction(ScalarFunction("st_aswkbpolyhedraltetra", {}, LogicalType::BLOB, ST_AsWKBPolyhedralTetraFun));
	loader.RegisterFunction(ScalarFunction("st_aswkbopentetra", {}, LogicalType::BLOB, ST_AsWKBOpenTetraFun));

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
	loader.RegisterFunction(ScalarFunction("st_3dvolume", {LogicalType::BLOB}, LogicalType::DOUBLE, ST_3DVolumeFun));

	// Accessor functions
	loader.RegisterFunction(ScalarFunction("st_ndims", {LogicalType::BLOB}, LogicalType::INTEGER, ST_NDimsFun));
	loader.RegisterFunction(ScalarFunction("st_hasz", {LogicalType::BLOB}, LogicalType::BOOLEAN, ST_HasZFun));
	loader.RegisterFunction(ScalarFunction("st_zmin", {LogicalType::BLOB}, LogicalType::DOUBLE, ST_ZMinFun));
	loader.RegisterFunction(ScalarFunction("st_zmax", {LogicalType::BLOB}, LogicalType::DOUBLE, ST_ZMaxFun));

	// Transform functions
	loader.RegisterFunction(ScalarFunction("st_translate",
	    {LogicalType::BLOB, LogicalType::DOUBLE, LogicalType::DOUBLE, LogicalType::DOUBLE},
	    LogicalType::BLOB, ST_TranslateFun));
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
