#include "functions/three_d_functions.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/function/function_set.hpp"
#include "duckdb/function/scalar_function.hpp"
#include "duckdb/common/vector/vector_iterator.hpp"
#include "duckdb/common/vector/vector_writer.hpp"

#include "kernel/geom_analysis.hpp"
#include "kernel/measurements.hpp"
#include "kernel/triangulation.hpp"

#include <cstdint>
#include <vector>

namespace duckdb {

// Kernel names this file uses unqualified. Using-declarations rather than a
// using-directive, which clang-tidy's google-build-using-namespace rejects.
using duckdb_3d::DeserializeGeomPayload;
using duckdb_3d::DeserializePayloadInto;
using duckdb_3d::ReadGeomPayloadHeader;
using duckdb_3d::ReadSolidPayloadHeader;

// ──────────────────────────────────────────────────────────────
// Introspection: ST_3DNumSolids, ST_3DNumShells, ST_3DNumFaces
// ──────────────────────────────────────────────────────────────
// These accessors only need element counts, which live in the fixed front
// header, so they read the header rather than deserialising the whole solid.
static void ST_3DNumSolidsFun(DataChunk &args, ExpressionState &state, Vector &result) {
	UnaryExecutor::Execute<string_t, int64_t>(args.data[0], result, [](string_t solid) {
		auto info = ReadSolidPayloadHeader(reinterpret_cast<const uint8_t *>(solid.GetData()), solid.GetSize());
		return static_cast<int64_t>(info.solid_count);
	});
}

static void ST_3DNumShellsFun(DataChunk &args, ExpressionState &state, Vector &result) {
	UnaryExecutor::Execute<string_t, int64_t>(args.data[0], result, [](string_t solid) {
		auto info = ReadSolidPayloadHeader(reinterpret_cast<const uint8_t *>(solid.GetData()), solid.GetSize());
		return static_cast<int64_t>(info.shell_count);
	});
}

static void ST_3DNumFacesFun(DataChunk &args, ExpressionState &state, Vector &result) {
	UnaryExecutor::Execute<string_t, int64_t>(args.data[0], result, [](string_t solid) {
		auto info = ReadSolidPayloadHeader(reinterpret_cast<const uint8_t *>(solid.GetData()), solid.GetSize());
		return static_cast<int64_t>(info.face_count);
	});
}

// ──────────────────────────────────────────────────────────────
// Validation: ST_3DIsClosed, ST_3DIsManifold, ST_3DIsOriented
// ──────────────────────────────────────────────────────────────
// The validation summary is cached in the payload (trailing block), so these
// read the header rather than re-running validation or parsing the body.
static void ST_3DIsClosedFun(DataChunk &args, ExpressionState &state, Vector &result) {
	UnaryExecutor::Execute<string_t, bool>(args.data[0], result, [](string_t solid) {
		auto info = ReadSolidPayloadHeader(reinterpret_cast<const uint8_t *>(solid.GetData()), solid.GetSize());
		return info.validation.is_closed;
	});
}

static void ST_3DIsValidFun(DataChunk &args, ExpressionState &state, Vector &result) {
	UnaryExecutor::Execute<string_t, bool>(args.data[0], result, [](string_t solid) {
		auto info = ReadSolidPayloadHeader(reinterpret_cast<const uint8_t *>(solid.GetData()), solid.GetSize());
		return info.validation.is_valid;
	});
}

static void ST_3DAreaGeomFun(DataChunk &args, ExpressionState &state, Vector &result) {
	UnaryExecutor::Execute<string_t, double>(args.data[0], result, [](string_t geom) {
		return Geom3DArea(DeserializeGeomPayload(reinterpret_cast<const uint8_t *>(geom.GetData()), geom.GetSize()));
	});
}

static void ST_3DPerimeterGeomFun(DataChunk &args, ExpressionState &state, Vector &result) {
	UnaryExecutor::Execute<string_t, double>(args.data[0], result, [](string_t geom) {
		return Geom3DPerimeter(
		    DeserializeGeomPayload(reinterpret_cast<const uint8_t *>(geom.GetData()), geom.GetSize()));
	});
}

static void ST_3DIsManifoldFun(DataChunk &args, ExpressionState &state, Vector &result) {
	UnaryExecutor::Execute<string_t, bool>(args.data[0], result, [](string_t solid) {
		auto info = ReadSolidPayloadHeader(reinterpret_cast<const uint8_t *>(solid.GetData()), solid.GetSize());
		return info.validation.is_manifold;
	});
}

static void ST_3DIsOrientedFun(DataChunk &args, ExpressionState &state, Vector &result) {
	UnaryExecutor::Execute<string_t, bool>(args.data[0], result, [](string_t solid) {
		auto info = ReadSolidPayloadHeader(reinterpret_cast<const uint8_t *>(solid.GetData()), solid.GetSize());
		return info.validation.is_oriented;
	});
}

// ──────────────────────────────────────────────────────────────
// ST_3DValidationReport
// ──────────────────────────────────────────────────────────────
//! The code and message ST_3DValidationReport reports for a validation summary.
static void DescribeValidation(const duckdb_3d::ValidationCache &vc, string &code, string &message) {
	if (vc.is_valid) {
		code = "VALID";
		message = "Valid solid";
		return;
	}
	std::vector<string> issues;
	if (!vc.is_closed) {
		issues.emplace_back("not closed");
	}
	if (vc.non_manifold_edge_count > 0) {
		issues.emplace_back("non-manifold edges");
	}
	if (vc.non_manifold_vertex_count > 0) {
		issues.emplace_back("non-manifold vertices");
	}
	if (!vc.is_oriented) {
		issues.emplace_back("orientation inconsistent");
	}
	if (vc.degenerate_face_count > 0) {
		issues.emplace_back("degenerate faces");
	}
	if (vc.disconnected_shell_count > 0) {
		issues.emplace_back("disconnected shells");
	}
	if (vc.self_intersection_count > 0) {
		issues.emplace_back("self-intersecting faces");
	}
	code = "INVALID";
	message = "Invalid solid: ";
	for (size_t j = 0; j < issues.size(); j++) {
		if (j > 0) {
			message += ", ";
		}
		message += issues[j];
	}
}

static void ST_3DValidationReportFun(DataChunk &args, ExpressionState &state, Vector &result) {
	auto count = args.size();
	auto writer =
	    FlatVector::Writer<VectorStructType<bool, bool, bool, bool, int64_t, int64_t, int64_t, int64_t, int64_t,
	                                        int64_t, int64_t, int64_t, int64_t, int64_t, string_t, string_t>>(result,
	                                                                                                          count);
	for (auto entry : args.data[0].Values<string_t>()) {
		if (!entry.IsValid()) {
			writer.WriteNull();
			continue;
		}
		auto &blob = entry.GetValue();
		// Both the validation summary and counts are header/trailer data, so the
		// report is served without materialising vertices or topology.
		auto info = ReadSolidPayloadHeader(reinterpret_cast<const uint8_t *>(blob.GetData()), blob.GetSize());
		auto &vc = info.validation;
		string code;
		string message;
		DescribeValidation(vc, code, message);
		writer.WriteValue([&](auto &is_valid, auto &is_closed, auto &is_manifold, auto &is_oriented, auto &solid_count,
		                      auto &shell_count, auto &face_count, auto &open_edges, auto &non_manifold_edges,
		                      auto &degenerate_faces, auto &orientation_errors, auto &non_manifold_vertices,
		                      auto &disconnected_shells, auto &self_intersections, auto &code_writer,
		                      auto &message_writer) {
			is_valid.WriteValue(vc.is_valid);
			is_closed.WriteValue(vc.is_closed);
			is_manifold.WriteValue(vc.is_manifold);
			is_oriented.WriteValue(vc.is_oriented);
			solid_count.WriteValue(static_cast<int64_t>(info.solid_count));
			shell_count.WriteValue(static_cast<int64_t>(info.shell_count));
			face_count.WriteValue(static_cast<int64_t>(info.face_count));
			open_edges.WriteValue(static_cast<int64_t>(vc.open_edge_count));
			non_manifold_edges.WriteValue(static_cast<int64_t>(vc.non_manifold_edge_count));
			degenerate_faces.WriteValue(static_cast<int64_t>(vc.degenerate_face_count));
			orientation_errors.WriteValue(static_cast<int64_t>(vc.orientation_error_count));
			non_manifold_vertices.WriteValue(static_cast<int64_t>(vc.non_manifold_vertex_count));
			disconnected_shells.WriteValue(static_cast<int64_t>(vc.disconnected_shell_count));
			self_intersections.WriteValue(static_cast<int64_t>(vc.self_intersection_count));
			code_writer.WriteValue(string_t(code));
			message_writer.WriteValue(string_t(message));
		});
	}
}

// ──────────────────────────────────────────────────────────────
// Measurements: ST_3DSurfaceArea, ST_3DVolume
// ──────────────────────────────────────────────────────────────
// Each measurement decodes its rows into one SolidModel reused across the
// chunk, so a row costs no allocation once the arrays have grown to fit.
static void ST_3DSurfaceAreaFun(DataChunk &args, ExpressionState &state, Vector &result) {
	duckdb_3d::SolidModel model;
	UnaryExecutor::Execute<string_t, double>(args.data[0], result, [&](string_t solid) {
		DeserializePayloadInto(reinterpret_cast<const uint8_t *>(solid.GetData()), solid.GetSize(), model);
		if (model.TriangleCount() == 0) {
			TriangulateSolidModel(model);
		}
		return ComputeSurfaceArea(model);
	});
}

static void ST_3DVolumeFun(DataChunk &args, ExpressionState &state, Vector &result) {
	duckdb_3d::SolidModel model;
	UnaryExecutor::Execute<string_t, double>(args.data[0], result, [&](string_t solid) {
		DeserializePayloadInto(reinterpret_cast<const uint8_t *>(solid.GetData()), solid.GetSize(), model);
		if (model.TriangleCount() == 0) {
			TriangulateSolidModel(model);
		}
		return ComputeVolume(model);
	});
}

static void ST_3DPerimeterFun(DataChunk &args, ExpressionState &state, Vector &result) {
	duckdb_3d::SolidModel model;
	UnaryExecutor::Execute<string_t, double>(args.data[0], result, [&](string_t solid) {
		DeserializePayloadInto(reinterpret_cast<const uint8_t *>(solid.GetData()), solid.GetSize(), model);
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
	UnaryExecutor::Execute<string_t, int32_t>(args.data[0], result,
	                                          [](string_t solid) { return CoordinateDimension3D(); });
}

static void ST_3DHasZFun(DataChunk &args, ExpressionState &state, Vector &result) {
	UnaryExecutor::Execute<string_t, bool>(args.data[0], result, [](string_t solid) {
		// v1 geometries always carry a Z ordinate.
		return true;
	});
}

// ST_3DFootprintArea accepts either a SOLID_3D (footprint of the solid) or a GEOM_3D
// (footprint of the geometry, e.g. a convex hull) — both the XY projection.
static void ST_3DFootprintAreaFun(DataChunk &args, ExpressionState &state, Vector &result) {
	duckdb_3d::SolidModel model;
	UnaryExecutor::Execute<string_t, double>(args.data[0], result, [&](string_t blob) {
		auto data = reinterpret_cast<const uint8_t *>(blob.GetData());
		auto size = blob.GetSize();
		switch (GetPayloadKind(data, size)) {
		case PayloadKind::Solid:
			DeserializePayloadInto(data, size, model);
			return ComputeFootprintArea(model);
		case PayloadKind::Geom:
			return Geom3DFootprintArea(DeserializeGeomPayload(data, size));
		default:
			throw InvalidInputException("ST_3DFootprintArea: argument is not a SOLID_3D or GEOM_3D value");
		}
	});
}

void RegisterSolidAccessorFunctions(ExtensionLoader &loader, const LogicalType &solid_3d_type,
                                    const LogicalType &geom_3d_type) {
	// Introspection: counts. SOLID_3D consumers take the alias only: a payload
	// read back from storage arrives as a plain BLOB, and the explicit
	// BLOB -> SOLID_3D cast (functions/casts.cpp) re-derives its caches before
	// any function trusts them.
	ScalarFunctionSet num_solids_set("st_3dnumsolids");
	num_solids_set.AddFunction(ScalarFunction({solid_3d_type}, LogicalType::BIGINT, ST_3DNumSolidsFun));
	RegisterDocumented(loader, std::move(num_solids_set),
	                   {{"solid"},
	                    "Returns the number of solids in a SOLID_3D value: 1 for a solid, more for a multi-solid. "
	                    "Reads the payload header only.",
	                    "ST_3DNumSolids(ST_3DExtrude(ST_Geom3DFromWKB('POLYGON Z ((0 0 0, 2 0 0, 2 2 0, 0 2 0, 0 0 "
	                    "0))'::GEOMETRY), 3.0))",
	                    {"introspection"}});

	ScalarFunctionSet num_shells_set("st_3dnumshells");
	num_shells_set.AddFunction(ScalarFunction({solid_3d_type}, LogicalType::BIGINT, ST_3DNumShellsFun));
	RegisterDocumented(loader, std::move(num_shells_set),
	                   {{"solid"},
	                    "Returns the number of shells (exterior and interior) across all solids of a SOLID_3D value. "
	                    "Reads the payload header only.",
	                    "ST_3DNumShells(ST_3DExtrude(ST_Geom3DFromWKB('POLYGON Z ((0 0 0, 2 0 0, 2 2 0, 0 2 0, 0 0 "
	                    "0))'::GEOMETRY), 3.0))",
	                    {"introspection"}});

	ScalarFunctionSet num_faces_set("st_3dnumfaces");
	num_faces_set.AddFunction(ScalarFunction({solid_3d_type}, LogicalType::BIGINT, ST_3DNumFacesFun));
	RegisterDocumented(
	    loader, std::move(num_faces_set),
	    {{"solid"},
	     "Returns the number of faces across all shells of a SOLID_3D value. Reads the payload header only.",
	     "ST_3DNumFaces(ST_3DExtrude(ST_Geom3DFromWKB('POLYGON Z ((0 0 0, 2 0 0, 2 2 0, 0 2 0, 0 0 0))'::GEOMETRY), "
	     "3.0))",
	     {"introspection"}});

	// Validation functions
	ScalarFunctionSet is_valid_set("st_3disvalid");
	is_valid_set.AddFunction(ScalarFunction({solid_3d_type}, LogicalType::BOOLEAN, ST_3DIsValidFun));
	RegisterDocumented(loader, std::move(is_valid_set),
	                   {{"solid"},
	                    "Returns whether a SOLID_3D value is valid: ST_3DValidationReport's is_valid, read from the "
	                    "cached verdict.",
	                    "ST_3DIsValid(ST_3DExtrude(ST_Geom3DFromWKB('POLYGON Z ((0 0 0, 2 0 0, 2 2 0, 0 2 0, 0 0 "
	                    "0))'::GEOMETRY), 3.0))",
	                    {"validation"}});
	ScalarFunctionSet is_closed_set("st_3disclosed");
	is_closed_set.AddFunction(ScalarFunction({solid_3d_type}, LogicalType::BOOLEAN, ST_3DIsClosedFun));
	RegisterDocumented(loader, std::move(is_closed_set),
	                   {{"solid"},
	                    "Returns whether every shell of a SOLID_3D value is closed: each undirected edge used exactly "
	                    "twice, in opposing directions. Read from the validation cached at import.",
	                    "ST_3DIsClosed(ST_3DExtrude(ST_Geom3DFromWKB('POLYGON Z ((0 0 0, 2 0 0, 2 2 0, 0 2 0, 0 0 "
	                    "0))'::GEOMETRY), 3.0))",
	                    {"validation"}});

	ScalarFunctionSet is_manifold_set("st_3dismanifold");
	is_manifold_set.AddFunction(ScalarFunction({solid_3d_type}, LogicalType::BOOLEAN, ST_3DIsManifoldFun));
	RegisterDocumented(loader, std::move(is_manifold_set),
	                   {{"solid"},
	                    "Returns whether a SOLID_3D value is manifold: no edge belongs to more than two faces, and the "
	                    "faces around each vertex form one fan. Read from the validation cached at import.",
	                    "ST_3DIsManifold(ST_3DExtrude(ST_Geom3DFromWKB('POLYGON Z ((0 0 0, 2 0 0, 2 2 0, 0 2 0, 0 0 "
	                    "0))'::GEOMETRY), 3.0))",
	                    {"validation"}});

	ScalarFunctionSet is_oriented_set("st_3disoriented");
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
	report_children.push_back({"non_manifold_vertex_count", LogicalTypeId::BIGINT});
	report_children.push_back({"disconnected_shell_count", LogicalTypeId::BIGINT});
	report_children.push_back({"self_intersection_count", LogicalTypeId::BIGINT});
	report_children.push_back({"code", LogicalTypeId::VARCHAR});
	report_children.push_back({"message", LogicalTypeId::VARCHAR});
	auto report_type = LogicalType::STRUCT(std::move(report_children));
	ScalarFunctionSet report_set("st_3dvalidationreport");
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
	area3d_set.AddFunction(ScalarFunction({solid_3d_type}, LogicalType::DOUBLE, ST_3DSurfaceAreaFun));
	area3d_set.AddFunction(ScalarFunction({geom_3d_type}, LogicalType::DOUBLE, ST_3DAreaGeomFun));
	RegisterDocumented(
	    loader, std::move(area3d_set),
	    {{"geom"},
	     "Returns the 3D area in input units: for a SOLID_3D, ST_3DSurfaceArea (raises on degenerate faces); for a "
	     "GEOM_3D, the total area of its polygons, holes excluded, and 0 for points and lines.",
	     "ST_3DArea(ST_3DExtrude(ST_Geom3DFromWKB('POLYGON Z ((0 0 0, 2 0 0, 2 2 0, 0 2 0, 0 0 0))'::GEOMETRY), 3.0))",
	     {"measurement"}});
	ScalarFunctionSet volume_set("st_3dvolume");
	volume_set.AddFunction(ScalarFunction({solid_3d_type}, LogicalType::DOUBLE, ST_3DVolumeFun));
	RegisterDocumented(
	    loader, std::move(volume_set),
	    {{"solid"},
	     "Returns the volume of a SOLID_3D value in input units; interior shells (cavities) subtract and multi-solids "
	     "sum. Raises unless the solid is closed, manifold, oriented and free of degenerate faces.",
	     "ST_3DVolume(ST_3DExtrude(ST_Geom3DFromWKB('POLYGON Z ((0 0 0, 2 0 0, 2 2 0, 0 2 0, 0 0 0))'::GEOMETRY), "
	     "3.0))",
	     {"measurement"}});
	// ST_3DFootprintArea dispatches by payload magic, so accept SOLID_3D and GEOM_3D
	// — same pattern as ST_3DZMin/ST_3DZMax.
	ScalarFunctionSet area_set("st_3dfootprintarea");
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
	perimeter_set.AddFunction(ScalarFunction({solid_3d_type}, LogicalType::DOUBLE, ST_3DPerimeterFun));
	perimeter_set.AddFunction(ScalarFunction({geom_3d_type}, LogicalType::DOUBLE, ST_3DPerimeterGeomFun));
	RegisterDocumented(loader, std::move(perimeter_set),
	                   {{"geom"},
	                    "Returns a 3D perimeter in input units: for a SOLID_3D, the total length of its boundary "
	                    "edges, those used by exactly one face (0 when closed); for a GEOM_3D, as PostGIS, the length "
	                    "of every ring of its polygons, and 0 for points and lines.",
	                    "ST_3DPerimeter(ST_3DExtrude(ST_Geom3DFromWKB('POLYGON Z ((0 0 0, 2 0 0, 2 2 0, 0 2 0, 0 0 "
	                    "0))'::GEOMETRY), 3.0))",
	                    {"measurement"}});

	// Accessor functions
	ScalarFunctionSet ndims_set("st_ndims");
	ndims_set.AddFunction(ScalarFunction({solid_3d_type}, LogicalType::INTEGER, ST_NDimsFun));
	ndims_set.AddFunction(ScalarFunction({geom_3d_type}, LogicalType::INTEGER, ST_NDimsFun));
	RegisterDocumented(loader, std::move(ndims_set),
	                   {{"geom"},
	                    "Returns the coordinate dimension of a SOLID_3D or GEOM_3D value, always 3. A constant for "
	                    "PostGIS compatibility; the payload is not read.",
	                    "ST_NDims(ST_Geom3DFromWKB('POINT Z (1 2 3)'::GEOMETRY))",
	                    {"introspection"},
	                    false});

	ScalarFunctionSet hasz_set("st_3dhasz");
	hasz_set.AddFunction(ScalarFunction({solid_3d_type}, LogicalType::BOOLEAN, ST_3DHasZFun));
	hasz_set.AddFunction(ScalarFunction({geom_3d_type}, LogicalType::BOOLEAN, ST_3DHasZFun));
	RegisterDocumented(loader, std::move(hasz_set),
	                   {{"geom"},
	                    "Returns whether a SOLID_3D or GEOM_3D value has Z coordinates, always true. A constant for "
	                    "PostGIS compatibility; the payload is not read.",
	                    "ST_3DHasZ(ST_Geom3DFromWKB('POINT Z (1 2 3)'::GEOMETRY))",
	                    {"introspection"},
	                    false});
}

} // namespace duckdb
