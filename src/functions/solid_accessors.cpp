#include "functions/three_d_functions.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/function/function_set.hpp"
#include "duckdb/function/scalar_function.hpp"

#include "kernel/geom_analysis.hpp"
#include "kernel/measurements.hpp"
#include "kernel/triangulation.hpp"

#include <cstdint>
#include <vector>

namespace duckdb {

// Kernel names this file uses unqualified. Using-declarations rather than a
// using-directive, which clang-tidy's google-build-using-namespace rejects.
using duckdb_3d::DeserializeGeomPayload;
using duckdb_3d::DeserializePayload;
using duckdb_3d::ReadGeomPayloadHeader;
using duckdb_3d::ReadSolidPayloadHeader;

// ──────────────────────────────────────────────────────────────
// Introspection: ST_3DNumSolids, ST_3DNumShells, ST_3DNumFaces
// ──────────────────────────────────────────────────────────────
// These accessors only need element counts, which live in the fixed front
// header, so they read the header rather than deserialising the whole solid.
static void ST_3DNumSolidsFun(DataChunk &args, ExpressionState &state, Vector &result) {
	UnaryExecutor::Execute<string_t, int64_t>(args.data[0], result, args.size(), [](string_t solid) {
		auto info = ReadSolidPayloadHeader(reinterpret_cast<const uint8_t *>(solid.GetData()), solid.GetSize());
		return static_cast<int64_t>(info.solid_count);
	});
}

static void ST_3DNumShellsFun(DataChunk &args, ExpressionState &state, Vector &result) {
	UnaryExecutor::Execute<string_t, int64_t>(args.data[0], result, args.size(), [](string_t solid) {
		auto info = ReadSolidPayloadHeader(reinterpret_cast<const uint8_t *>(solid.GetData()), solid.GetSize());
		return static_cast<int64_t>(info.shell_count);
	});
}

static void ST_3DNumFacesFun(DataChunk &args, ExpressionState &state, Vector &result) {
	UnaryExecutor::Execute<string_t, int64_t>(args.data[0], result, args.size(), [](string_t solid) {
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
		auto info = ReadSolidPayloadHeader(reinterpret_cast<const uint8_t *>(solid.GetData()), solid.GetSize());
		return info.validation.is_closed;
	});
}

static void ST_3DIsManifoldFun(DataChunk &args, ExpressionState &state, Vector &result) {
	UnaryExecutor::Execute<string_t, bool>(args.data[0], result, args.size(), [](string_t solid) {
		auto info = ReadSolidPayloadHeader(reinterpret_cast<const uint8_t *>(solid.GetData()), solid.GetSize());
		return info.validation.is_manifold;
	});
}

static void ST_3DIsOrientedFun(DataChunk &args, ExpressionState &state, Vector &result) {
	UnaryExecutor::Execute<string_t, bool>(args.data[0], result, args.size(), [](string_t solid) {
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
			if (!vc.is_closed) {
				issues.emplace_back("not closed");
			}
			if (!vc.is_manifold) {
				issues.emplace_back("non-manifold edges");
			}
			if (!vc.is_oriented) {
				issues.emplace_back("orientation inconsistent");
			}
			if (vc.degenerate_face_count > 0) {
				issues.emplace_back("degenerate faces");
			}
			code_str = "INVALID";
			msg_str = "Invalid solid: ";
			for (size_t j = 0; j < issues.size(); j++) {
				if (j > 0) {
					msg_str += ", ";
				}
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
		auto model = DeserializePayload(reinterpret_cast<const uint8_t *>(solid.GetData()), solid.GetSize());
		if (model.TriangleCount() == 0) {
			TriangulateSolidModel(model);
		}
		return ComputeSurfaceArea(model);
	});
}

static void ST_3DVolumeFun(DataChunk &args, ExpressionState &state, Vector &result) {
	UnaryExecutor::Execute<string_t, double>(args.data[0], result, args.size(), [](string_t solid) {
		auto model = DeserializePayload(reinterpret_cast<const uint8_t *>(solid.GetData()), solid.GetSize());
		if (model.TriangleCount() == 0) {
			TriangulateSolidModel(model);
		}
		return ComputeVolume(model);
	});
}

static void ST_3DPerimeterFun(DataChunk &args, ExpressionState &state, Vector &result) {
	UnaryExecutor::Execute<string_t, double>(args.data[0], result, args.size(), [](string_t solid) {
		auto model = DeserializePayload(reinterpret_cast<const uint8_t *>(solid.GetData()), solid.GetSize());
		return ComputePerimeter(model);
	});
}

// ──────────────────────────────────────────────────────────────
// Accessors: ST_NDims
// ──────────────────────────────────────────────────────────────
int32_t CoordinateDimension3D() {
	// v1 stores transformed XYZ coordinates only.
	return 3;
}

static void ST_NDimsFun(DataChunk &args, ExpressionState &state, Vector &result) {
	UnaryExecutor::Execute<string_t, int32_t>(args.data[0], result, args.size(),
	                                          [](string_t solid) { return CoordinateDimension3D(); });
}

static void ST_3DHasZFun(DataChunk &args, ExpressionState &state, Vector &result) {
	UnaryExecutor::Execute<string_t, bool>(args.data[0], result, args.size(), [](string_t solid) {
		// v1 geometries always carry a Z ordinate.
		return true;
	});
}

static void ST_3DZMinFun(DataChunk &args, ExpressionState &state, Vector &result) {
	UnaryExecutor::Execute<string_t, double>(args.data[0], result, args.size(), [](string_t blob) {
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

static void ST_3DZMaxFun(DataChunk &args, ExpressionState &state, Vector &result) {
	UnaryExecutor::Execute<string_t, double>(args.data[0], result, args.size(), [](string_t blob) {
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
static void ST_3DFootprintAreaFun(DataChunk &args, ExpressionState &state, Vector &result) {
	UnaryExecutor::Execute<string_t, double>(args.data[0], result, args.size(), [](string_t blob) {
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

void RegisterSolidAccessorFunctions(ExtensionLoader &loader, const LogicalType &solid_3d_type,
                                    const LogicalType &geom_3d_type) {
	// Introspection: counts. Every SOLID_3D consumer carries both a SOLID_3D and a
	// plain-BLOB overload: the constructors now return the alias (so the typed
	// overload binds without a cast), while the BLOB overload keeps stored and
	// legacy payloads working. DuckDB treats a named alias as distinct from its
	// base type for function resolution, hence the pair rather than one entry.
	ScalarFunctionSet num_solids_set("st_3dnumsolids");
	num_solids_set.AddFunction(ScalarFunction({LogicalType::BLOB}, LogicalType::BIGINT, ST_3DNumSolidsFun));
	num_solids_set.AddFunction(ScalarFunction({solid_3d_type}, LogicalType::BIGINT, ST_3DNumSolidsFun));
	loader.RegisterFunction(num_solids_set);

	ScalarFunctionSet num_shells_set("st_3dnumshells");
	num_shells_set.AddFunction(ScalarFunction({LogicalType::BLOB}, LogicalType::BIGINT, ST_3DNumShellsFun));
	num_shells_set.AddFunction(ScalarFunction({solid_3d_type}, LogicalType::BIGINT, ST_3DNumShellsFun));
	loader.RegisterFunction(num_shells_set);

	ScalarFunctionSet num_faces_set("st_3dnumfaces");
	num_faces_set.AddFunction(ScalarFunction({LogicalType::BLOB}, LogicalType::BIGINT, ST_3DNumFacesFun));
	num_faces_set.AddFunction(ScalarFunction({solid_3d_type}, LogicalType::BIGINT, ST_3DNumFacesFun));
	loader.RegisterFunction(num_faces_set);

	// Introspection: bounds
	child_list_t<LogicalType> bbox_children;
	bbox_children.push_back({"min_x", LogicalType::DOUBLE});
	bbox_children.push_back({"min_y", LogicalType::DOUBLE});
	bbox_children.push_back({"min_z", LogicalType::DOUBLE});
	bbox_children.push_back({"max_x", LogicalType::DOUBLE});
	bbox_children.push_back({"max_y", LogicalType::DOUBLE});
	bbox_children.push_back({"max_z", LogicalType::DOUBLE});
	auto bbox_type = LogicalType::STRUCT(std::move(bbox_children));
	ScalarFunctionSet bounds_set("st_3dbounds");
	bounds_set.AddFunction(ScalarFunction({LogicalType::BLOB}, bbox_type, ST_3DBoundsFun));
	bounds_set.AddFunction(ScalarFunction({solid_3d_type}, bbox_type, ST_3DBoundsFun));
	loader.RegisterFunction(bounds_set);

	// Validation functions
	ScalarFunctionSet is_closed_set("st_3disclosed");
	is_closed_set.AddFunction(ScalarFunction({LogicalType::BLOB}, LogicalType::BOOLEAN, ST_3DIsClosedFun));
	is_closed_set.AddFunction(ScalarFunction({solid_3d_type}, LogicalType::BOOLEAN, ST_3DIsClosedFun));
	loader.RegisterFunction(is_closed_set);

	ScalarFunctionSet is_manifold_set("st_3dismanifold");
	is_manifold_set.AddFunction(ScalarFunction({LogicalType::BLOB}, LogicalType::BOOLEAN, ST_3DIsManifoldFun));
	is_manifold_set.AddFunction(ScalarFunction({solid_3d_type}, LogicalType::BOOLEAN, ST_3DIsManifoldFun));
	loader.RegisterFunction(is_manifold_set);

	ScalarFunctionSet is_oriented_set("st_3disoriented");
	is_oriented_set.AddFunction(ScalarFunction({LogicalType::BLOB}, LogicalType::BOOLEAN, ST_3DIsOrientedFun));
	is_oriented_set.AddFunction(ScalarFunction({solid_3d_type}, LogicalType::BOOLEAN, ST_3DIsOrientedFun));
	loader.RegisterFunction(is_oriented_set);

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
	ScalarFunctionSet report_set("st_3dvalidationreport");
	report_set.AddFunction(ScalarFunction({LogicalType::BLOB}, report_type, ST_3DValidationReportFun));
	report_set.AddFunction(ScalarFunction({solid_3d_type}, report_type, ST_3DValidationReportFun));
	loader.RegisterFunction(report_set);

	// Measurement functions
	ScalarFunctionSet surface_area_set("st_3dsurfacearea");
	surface_area_set.AddFunction(ScalarFunction({LogicalType::BLOB}, LogicalType::DOUBLE, ST_3DSurfaceAreaFun));
	surface_area_set.AddFunction(ScalarFunction({solid_3d_type}, LogicalType::DOUBLE, ST_3DSurfaceAreaFun));
	loader.RegisterFunction(surface_area_set);
	// ST_3DArea is the surface-area measurement under a PostGIS-aligned name.
	ScalarFunctionSet area3d_set("st_3darea");
	area3d_set.AddFunction(ScalarFunction({LogicalType::BLOB}, LogicalType::DOUBLE, ST_3DSurfaceAreaFun));
	area3d_set.AddFunction(ScalarFunction({solid_3d_type}, LogicalType::DOUBLE, ST_3DSurfaceAreaFun));
	loader.RegisterFunction(area3d_set);
	ScalarFunctionSet volume_set("st_3dvolume");
	volume_set.AddFunction(ScalarFunction({LogicalType::BLOB}, LogicalType::DOUBLE, ST_3DVolumeFun));
	volume_set.AddFunction(ScalarFunction({solid_3d_type}, LogicalType::DOUBLE, ST_3DVolumeFun));
	loader.RegisterFunction(volume_set);
	// ST_3DFootprintArea dispatches by payload magic, so accept SOLID_3D and GEOM_3D (and
	// raw BLOB) — same pattern as ST_3DZMin/ST_3DZMax.
	ScalarFunctionSet area_set("st_3dfootprintarea");
	area_set.AddFunction(ScalarFunction({LogicalType::BLOB}, LogicalType::DOUBLE, ST_3DFootprintAreaFun));
	area_set.AddFunction(ScalarFunction({solid_3d_type}, LogicalType::DOUBLE, ST_3DFootprintAreaFun));
	area_set.AddFunction(ScalarFunction({geom_3d_type}, LogicalType::DOUBLE, ST_3DFootprintAreaFun));
	loader.RegisterFunction(area_set);
	ScalarFunctionSet perimeter_set("st_3dperimeter");
	perimeter_set.AddFunction(ScalarFunction({LogicalType::BLOB}, LogicalType::DOUBLE, ST_3DPerimeterFun));
	perimeter_set.AddFunction(ScalarFunction({solid_3d_type}, LogicalType::DOUBLE, ST_3DPerimeterFun));
	loader.RegisterFunction(perimeter_set);

	// Accessor functions
	ScalarFunctionSet ndims_set("st_ndims");
	ndims_set.AddFunction(ScalarFunction({LogicalType::BLOB}, LogicalType::INTEGER, ST_NDimsFun));
	ndims_set.AddFunction(ScalarFunction({solid_3d_type}, LogicalType::INTEGER, ST_NDimsFun));
	ndims_set.AddFunction(ScalarFunction({geom_3d_type}, LogicalType::INTEGER, ST_NDimsFun));
	loader.RegisterFunction(ndims_set);

	ScalarFunctionSet hasz_set("st_3dhasz");
	hasz_set.AddFunction(ScalarFunction({LogicalType::BLOB}, LogicalType::BOOLEAN, ST_3DHasZFun));
	hasz_set.AddFunction(ScalarFunction({solid_3d_type}, LogicalType::BOOLEAN, ST_3DHasZFun));
	hasz_set.AddFunction(ScalarFunction({geom_3d_type}, LogicalType::BOOLEAN, ST_3DHasZFun));
	loader.RegisterFunction(hasz_set);

	// ST_3DZMin / ST_3DZMax: class-generic bbox accessors, accept SOLID_3D, GEOM_3D,
	// and plain BLOB values. Multiple overloads are needed because DuckDB treats
	// named type aliases as distinct for function resolution.
	ScalarFunctionSet zmin_set("st_3dzmin");
	zmin_set.AddFunction(ScalarFunction({LogicalType::BLOB}, LogicalType::DOUBLE, ST_3DZMinFun));
	zmin_set.AddFunction(ScalarFunction({solid_3d_type}, LogicalType::DOUBLE, ST_3DZMinFun));
	zmin_set.AddFunction(ScalarFunction({geom_3d_type}, LogicalType::DOUBLE, ST_3DZMinFun));
	loader.RegisterFunction(zmin_set);

	ScalarFunctionSet zmax_set("st_3dzmax");
	zmax_set.AddFunction(ScalarFunction({LogicalType::BLOB}, LogicalType::DOUBLE, ST_3DZMaxFun));
	zmax_set.AddFunction(ScalarFunction({solid_3d_type}, LogicalType::DOUBLE, ST_3DZMaxFun));
	zmax_set.AddFunction(ScalarFunction({geom_3d_type}, LogicalType::DOUBLE, ST_3DZMaxFun));
	loader.RegisterFunction(zmax_set);
}

} // namespace duckdb
