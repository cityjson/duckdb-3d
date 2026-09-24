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
	RegisterDocumented(loader, std::move(num_solids_set),
	                   {{"solid"},
	                    "Returns the number of solids in a SOLID_3D value: 1 for a solid, more for a multi-solid. "
	                    "Reads the payload header only.",
	                    "ST_3DNumSolids(ST_3DExtrude(ST_Geom3DFromWKB('POLYGON Z ((0 0 0, 2 0 0, 2 2 0, 0 2 0, 0 0 "
	                    "0))'::GEOMETRY), 3.0))",
	                    {"introspection"}});

	ScalarFunctionSet num_shells_set("st_3dnumshells");
	num_shells_set.AddFunction(ScalarFunction({LogicalType::BLOB}, LogicalType::BIGINT, ST_3DNumShellsFun));
	num_shells_set.AddFunction(ScalarFunction({solid_3d_type}, LogicalType::BIGINT, ST_3DNumShellsFun));
	RegisterDocumented(loader, std::move(num_shells_set),
	                   {{"solid"},
	                    "Returns the number of shells (exterior and interior) across all solids of a SOLID_3D value. "
	                    "Reads the payload header only.",
	                    "ST_3DNumShells(ST_3DExtrude(ST_Geom3DFromWKB('POLYGON Z ((0 0 0, 2 0 0, 2 2 0, 0 2 0, 0 0 "
	                    "0))'::GEOMETRY), 3.0))",
	                    {"introspection"}});

	ScalarFunctionSet num_faces_set("st_3dnumfaces");
	num_faces_set.AddFunction(ScalarFunction({LogicalType::BLOB}, LogicalType::BIGINT, ST_3DNumFacesFun));
	num_faces_set.AddFunction(ScalarFunction({solid_3d_type}, LogicalType::BIGINT, ST_3DNumFacesFun));
	RegisterDocumented(
	    loader, std::move(num_faces_set),
	    {{"solid"},
	     "Returns the number of faces across all shells of a SOLID_3D value. Reads the payload header only.",
	     "ST_3DNumFaces(ST_3DExtrude(ST_Geom3DFromWKB('POLYGON Z ((0 0 0, 2 0 0, 2 2 0, 0 2 0, 0 0 0))'::GEOMETRY), "
	     "3.0))",
	     {"introspection"}});

	// Introspection: bounds. Struct children name LogicalTypeId, not the
	// LogicalType:: constants: pair's forwarding constructor would bind a
	// reference to the constexpr member, which C++17 emits as an inline variable
	// here and DuckDB (C++11) defines out of line, and the static link then sees
	// two definitions.
	child_list_t<LogicalType> bbox_children;
	bbox_children.push_back({"min_x", LogicalTypeId::DOUBLE});
	bbox_children.push_back({"min_y", LogicalTypeId::DOUBLE});
	bbox_children.push_back({"min_z", LogicalTypeId::DOUBLE});
	bbox_children.push_back({"max_x", LogicalTypeId::DOUBLE});
	bbox_children.push_back({"max_y", LogicalTypeId::DOUBLE});
	bbox_children.push_back({"max_z", LogicalTypeId::DOUBLE});
	auto bbox_type = LogicalType::STRUCT(std::move(bbox_children));
	ScalarFunctionSet bounds_set("st_3dbounds");
	bounds_set.AddFunction(ScalarFunction({LogicalType::BLOB}, bbox_type, ST_3DBoundsFun));
	bounds_set.AddFunction(ScalarFunction({solid_3d_type}, bbox_type, ST_3DBoundsFun));
	RegisterDocumented(loader, std::move(bounds_set),
	                   {{"solid"},
	                    "Returns the 3D bounding box of a SOLID_3D value as STRUCT(min_x, min_y, min_z, max_x, max_y, "
	                    "max_z), read from the cached box.",
	                    "ST_3DBounds(ST_3DExtrude(ST_Geom3DFromWKB('POLYGON Z ((0 0 0, 2 0 0, 2 2 0, 0 2 0, 0 0 "
	                    "0))'::GEOMETRY), 3.0))",
	                    {"introspection"}});

	// Validation functions
	ScalarFunctionSet is_closed_set("st_3disclosed");
	is_closed_set.AddFunction(ScalarFunction({LogicalType::BLOB}, LogicalType::BOOLEAN, ST_3DIsClosedFun));
	is_closed_set.AddFunction(ScalarFunction({solid_3d_type}, LogicalType::BOOLEAN, ST_3DIsClosedFun));
	RegisterDocumented(loader, std::move(is_closed_set),
	                   {{"solid"},
	                    "Returns whether every shell of a SOLID_3D value is closed: each undirected edge used exactly "
	                    "twice, in opposing directions. Read from the validation cached at import.",
	                    "ST_3DIsClosed(ST_3DExtrude(ST_Geom3DFromWKB('POLYGON Z ((0 0 0, 2 0 0, 2 2 0, 0 2 0, 0 0 "
	                    "0))'::GEOMETRY), 3.0))",
	                    {"validation"}});

	ScalarFunctionSet is_manifold_set("st_3dismanifold");
	is_manifold_set.AddFunction(ScalarFunction({LogicalType::BLOB}, LogicalType::BOOLEAN, ST_3DIsManifoldFun));
	is_manifold_set.AddFunction(ScalarFunction({solid_3d_type}, LogicalType::BOOLEAN, ST_3DIsManifoldFun));
	RegisterDocumented(loader, std::move(is_manifold_set),
	                   {{"solid"},
	                    "Returns whether a SOLID_3D value is manifold: no edge belongs to more than two faces. Read "
	                    "from the validation cached at import.",
	                    "ST_3DIsManifold(ST_3DExtrude(ST_Geom3DFromWKB('POLYGON Z ((0 0 0, 2 0 0, 2 2 0, 0 2 0, 0 0 "
	                    "0))'::GEOMETRY), 3.0))",
	                    {"validation"}});

	ScalarFunctionSet is_oriented_set("st_3disoriented");
	is_oriented_set.AddFunction(ScalarFunction({LogicalType::BLOB}, LogicalType::BOOLEAN, ST_3DIsOrientedFun));
	is_oriented_set.AddFunction(ScalarFunction({solid_3d_type}, LogicalType::BOOLEAN, ST_3DIsOrientedFun));
	RegisterDocumented(loader, std::move(is_oriented_set),
	                   {{"solid"},
	                    "Returns whether face winding in a SOLID_3D value is consistent within each shell, with "
	                    "interior shells wound opposite the exterior. Read from the validation cached at import.",
	                    "ST_3DIsOriented(ST_3DExtrude(ST_Geom3DFromWKB('POLYGON Z ((0 0 0, 2 0 0, 2 2 0, 0 2 0, 0 0 "
	                    "0))'::GEOMETRY), 3.0))",
	                    {"validation"}});

	// Validation report
	child_list_t<LogicalType> report_children;
	report_children.push_back({"is_valid", LogicalTypeId::BOOLEAN});
	report_children.push_back({"is_closed", LogicalTypeId::BOOLEAN});
	report_children.push_back({"is_manifold", LogicalTypeId::BOOLEAN});
	report_children.push_back({"is_oriented", LogicalTypeId::BOOLEAN});
	report_children.push_back({"solid_count", LogicalTypeId::BIGINT});
	report_children.push_back({"shell_count", LogicalTypeId::BIGINT});
	report_children.push_back({"face_count", LogicalTypeId::BIGINT});
	report_children.push_back({"open_edge_count", LogicalTypeId::BIGINT});
	report_children.push_back({"non_manifold_edge_count", LogicalTypeId::BIGINT});
	report_children.push_back({"degenerate_face_count", LogicalTypeId::BIGINT});
	report_children.push_back({"orientation_error_count", LogicalTypeId::BIGINT});
	report_children.push_back({"code", LogicalTypeId::VARCHAR});
	report_children.push_back({"message", LogicalTypeId::VARCHAR});
	auto report_type = LogicalType::STRUCT(std::move(report_children));
	ScalarFunctionSet report_set("st_3dvalidationreport");
	report_set.AddFunction(ScalarFunction({LogicalType::BLOB}, report_type, ST_3DValidationReportFun));
	report_set.AddFunction(ScalarFunction({solid_3d_type}, report_type, ST_3DValidationReportFun));
	RegisterDocumented(loader, std::move(report_set),
	                   {{"solid"},
	                    "Returns a STRUCT reporting a SOLID_3D value's validity: the overall and per-check flags, "
	                    "solid/shell/face counts, per-defect counters, a code ('VALID' or 'INVALID') and a message.",
	                    "ST_3DValidationReport(ST_3DExtrude(ST_Geom3DFromWKB('POLYGON Z ((0 0 0, 2 0 0, 2 2 0, 0 2 0, "
	                    "0 0 0))'::GEOMETRY), 3.0))",
	                    {"validation"}});

	// Measurement functions
	ScalarFunctionSet surface_area_set("st_3dsurfacearea");
	surface_area_set.AddFunction(ScalarFunction({LogicalType::BLOB}, LogicalType::DOUBLE, ST_3DSurfaceAreaFun));
	surface_area_set.AddFunction(ScalarFunction({solid_3d_type}, LogicalType::DOUBLE, ST_3DSurfaceAreaFun));
	RegisterDocumented(loader, std::move(surface_area_set),
	                   {{"solid"},
	                    "Returns the total area of all faces of a SOLID_3D value in input units, interior-shell faces "
	                    "included. Raises on degenerate faces.",
	                    "ST_3DSurfaceArea(ST_3DExtrude(ST_Geom3DFromWKB('POLYGON Z ((0 0 0, 2 0 0, 2 2 0, 0 2 0, 0 0 "
	                    "0))'::GEOMETRY), 3.0))",
	                    {"measurement"}});
	// ST_3DArea is the surface-area measurement under a PostGIS-aligned name.
	ScalarFunctionSet area3d_set("st_3darea");
	area3d_set.AddFunction(ScalarFunction({LogicalType::BLOB}, LogicalType::DOUBLE, ST_3DSurfaceAreaFun));
	area3d_set.AddFunction(ScalarFunction({solid_3d_type}, LogicalType::DOUBLE, ST_3DSurfaceAreaFun));
	RegisterDocumented(
	    loader, std::move(area3d_set),
	    {{"solid"},
	     "Alias of ST_3DSurfaceArea: the total area of all faces of a SOLID_3D value in input units. Raises on "
	     "degenerate faces.",
	     "ST_3DArea(ST_3DExtrude(ST_Geom3DFromWKB('POLYGON Z ((0 0 0, 2 0 0, 2 2 0, 0 2 0, 0 0 0))'::GEOMETRY), 3.0))",
	     {"measurement"}});
	ScalarFunctionSet volume_set("st_3dvolume");
	volume_set.AddFunction(ScalarFunction({LogicalType::BLOB}, LogicalType::DOUBLE, ST_3DVolumeFun));
	volume_set.AddFunction(ScalarFunction({solid_3d_type}, LogicalType::DOUBLE, ST_3DVolumeFun));
	RegisterDocumented(
	    loader, std::move(volume_set),
	    {{"solid"},
	     "Returns the volume of a SOLID_3D value in input units; interior shells (cavities) subtract and multi-solids "
	     "sum. Raises unless the solid is closed, manifold, oriented and free of degenerate faces.",
	     "ST_3DVolume(ST_3DExtrude(ST_Geom3DFromWKB('POLYGON Z ((0 0 0, 2 0 0, 2 2 0, 0 2 0, 0 0 0))'::GEOMETRY), "
	     "3.0))",
	     {"measurement"}});
	// ST_3DFootprintArea dispatches by payload magic, so accept SOLID_3D and GEOM_3D (and
	// raw BLOB) — same pattern as ST_3DZMin/ST_3DZMax.
	ScalarFunctionSet area_set("st_3dfootprintarea");
	area_set.AddFunction(ScalarFunction({LogicalType::BLOB}, LogicalType::DOUBLE, ST_3DFootprintAreaFun));
	area_set.AddFunction(ScalarFunction({solid_3d_type}, LogicalType::DOUBLE, ST_3DFootprintAreaFun));
	area_set.AddFunction(ScalarFunction({geom_3d_type}, LogicalType::DOUBLE, ST_3DFootprintAreaFun));
	RegisterDocumented(loader, std::move(area_set),
	                   {{"geom"},
	                    "Returns the area of a SOLID_3D or GEOM_3D value projected onto the XY plane, in input units. "
	                    "Has no validity precondition.",
	                    "ST_3DFootprintArea(ST_3DExtrude(ST_Geom3DFromWKB('POLYGON Z ((0 0 0, 2 0 0, 2 2 0, 0 2 0, 0 0 "
	                    "0))'::GEOMETRY), 3.0))",
	                    {"measurement"}});
	ScalarFunctionSet perimeter_set("st_3dperimeter");
	perimeter_set.AddFunction(ScalarFunction({LogicalType::BLOB}, LogicalType::DOUBLE, ST_3DPerimeterFun));
	perimeter_set.AddFunction(ScalarFunction({solid_3d_type}, LogicalType::DOUBLE, ST_3DPerimeterFun));
	RegisterDocumented(loader, std::move(perimeter_set),
	                   {{"solid"},
	                    "Returns the total length of a SOLID_3D value's boundary edges, those used by exactly one "
	                    "face; a closed solid returns 0.",
	                    "ST_3DPerimeter(ST_3DExtrude(ST_Geom3DFromWKB('POLYGON Z ((0 0 0, 2 0 0, 2 2 0, 0 2 0, 0 0 "
	                    "0))'::GEOMETRY), 3.0))",
	                    {"measurement"}});

	// Accessor functions
	ScalarFunctionSet ndims_set("st_ndims");
	ndims_set.AddFunction(ScalarFunction({LogicalType::BLOB}, LogicalType::INTEGER, ST_NDimsFun));
	ndims_set.AddFunction(ScalarFunction({solid_3d_type}, LogicalType::INTEGER, ST_NDimsFun));
	ndims_set.AddFunction(ScalarFunction({geom_3d_type}, LogicalType::INTEGER, ST_NDimsFun));
	RegisterDocumented(loader, std::move(ndims_set),
	                   {{"geom"},
	                    "Returns the coordinate dimension of a SOLID_3D or GEOM_3D value, always 3. A constant for "
	                    "PostGIS compatibility; the payload is not read.",
	                    "ST_NDims(ST_Geom3DFromWKB('POINT Z (1 2 3)'::GEOMETRY))",
	                    {"introspection"}});

	ScalarFunctionSet hasz_set("st_3dhasz");
	hasz_set.AddFunction(ScalarFunction({LogicalType::BLOB}, LogicalType::BOOLEAN, ST_3DHasZFun));
	hasz_set.AddFunction(ScalarFunction({solid_3d_type}, LogicalType::BOOLEAN, ST_3DHasZFun));
	hasz_set.AddFunction(ScalarFunction({geom_3d_type}, LogicalType::BOOLEAN, ST_3DHasZFun));
	RegisterDocumented(loader, std::move(hasz_set),
	                   {{"geom"},
	                    "Returns whether a SOLID_3D or GEOM_3D value has Z coordinates, always true. A constant for "
	                    "PostGIS compatibility; the payload is not read.",
	                    "ST_3DHasZ(ST_Geom3DFromWKB('POINT Z (1 2 3)'::GEOMETRY))",
	                    {"introspection"}});

	// ST_3DZMin / ST_3DZMax: class-generic bbox accessors, accept SOLID_3D, GEOM_3D,
	// and plain BLOB values. Multiple overloads are needed because DuckDB treats
	// named type aliases as distinct for function resolution.
	ScalarFunctionSet zmin_set("st_3dzmin");
	zmin_set.AddFunction(ScalarFunction({LogicalType::BLOB}, LogicalType::DOUBLE, ST_3DZMinFun));
	zmin_set.AddFunction(ScalarFunction({solid_3d_type}, LogicalType::DOUBLE, ST_3DZMinFun));
	zmin_set.AddFunction(ScalarFunction({geom_3d_type}, LogicalType::DOUBLE, ST_3DZMinFun));
	RegisterDocumented(
	    loader, std::move(zmin_set),
	    {{"geom"},
	     "Returns the minimum Z coordinate of a SOLID_3D or GEOM_3D value, read from its cached bounding box.",
	     "ST_3DZMin(ST_3DExtrude(ST_Geom3DFromWKB('POLYGON Z ((0 0 0, 2 0 0, 2 2 0, 0 2 0, 0 0 0))'::GEOMETRY), 3.0))",
	     {"introspection"}});

	ScalarFunctionSet zmax_set("st_3dzmax");
	zmax_set.AddFunction(ScalarFunction({LogicalType::BLOB}, LogicalType::DOUBLE, ST_3DZMaxFun));
	zmax_set.AddFunction(ScalarFunction({solid_3d_type}, LogicalType::DOUBLE, ST_3DZMaxFun));
	zmax_set.AddFunction(ScalarFunction({geom_3d_type}, LogicalType::DOUBLE, ST_3DZMaxFun));
	RegisterDocumented(
	    loader, std::move(zmax_set),
	    {{"geom"},
	     "Returns the maximum Z coordinate of a SOLID_3D or GEOM_3D value, read from its cached bounding box.",
	     "ST_3DZMax(ST_3DExtrude(ST_Geom3DFromWKB('POLYGON Z ((0 0 0, 2 0 0, 2 2 0, 0 2 0, 0 0 0))'::GEOMETRY), 3.0))",
	     {"introspection"}});
}

} // namespace duckdb
