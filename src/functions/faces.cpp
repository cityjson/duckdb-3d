#include "functions/three_d_functions.hpp"

#include "duckdb/function/function_set.hpp"
#include "duckdb/function/scalar_function.hpp"

#include "kernel/faces.hpp"

#include <stdexcept>
#include <vector>

namespace duckdb {

// ──────────────────────────────────────────────────────────────
// Faces: ST_3DFaces, ST_3DFaceN, ST_3DFaceAreas, ST_3DFaceNormals,
// ST_3DFaceSlopes, ST_3DFaceAzimuths
// ──────────────────────────────────────────────────────────────
// A solid's faces are numbered from 1 in storage order, the order ST_3DAsWKB
// writes them; a GEOM_3D's faces are its polygons.

//! Face `face` of a solid as a GEOM_3D payload, or false when the face admits
//! no tiling and so cannot be a GEOM_3D polygon.
static bool WriteFace(Vector &result, const duckdb_3d::SolidModel &model, uint32_t face, string_t &out) {
	std::vector<uint8_t> payload;
	try {
		payload = duckdb_3d::SerializeGeomPayload(duckdb_3d::SolidFacePolygon(model, face));
	} catch (std::invalid_argument &) {
		return false;
	}
	out =
	    StringVector::AddStringOrBlob(result, string_t(reinterpret_cast<const char *>(payload.data()), payload.size()));
	return true;
}

static void ST_3DFacesFun(DataChunk &args, ExpressionState &state, Vector &result) {
	auto count = args.size();
	UnifiedVectorFormat input;
	args.data[0].ToUnifiedFormat(count, input);
	auto values = UnifiedVectorFormat::GetData<string_t>(input);
	auto list_entries = FlatVector::GetData<list_entry_t>(result);
	auto &child = ListVector::GetEntry(result);
	auto &fields = StructVector::GetEntries(child);
	duckdb_3d::SolidModel model;
	idx_t offset = 0;
	for (idx_t i = 0; i < count; i++) {
		auto idx = input.sel->get_index(i);
		if (!input.validity.RowIsValid(idx)) {
			FlatVector::SetNull(result, i, true);
			list_entries[i] = {offset, 0};
			continue;
		}
		duckdb_3d::DeserializePayloadInto(reinterpret_cast<const uint8_t *>(values[idx].GetData()),
		                                  values[idx].GetSize(), model);
		const idx_t faces = model.FaceCount();
		ListVector::Reserve(result, offset + faces);
		auto face_data = FlatVector::GetData<int32_t>(*fields[0]);
		auto solid_data = FlatVector::GetData<int32_t>(*fields[1]);
		auto shell_data = FlatVector::GetData<int32_t>(*fields[2]);
		auto polygon_data = FlatVector::GetData<string_t>(*fields[3]);
		for (uint32_t solid = 0; solid < model.SolidCount(); solid++) {
			const uint32_t first_shell = model.solid_shell_offsets[solid];
			for (uint32_t shell = first_shell; shell < model.solid_shell_offsets[solid + 1]; shell++) {
				for (uint32_t f = model.shell_face_offsets[shell]; f < model.shell_face_offsets[shell + 1]; f++) {
					const idx_t row = offset + f;
					face_data[row] = static_cast<int32_t>(f + 1);
					solid_data[row] = static_cast<int32_t>(solid + 1);
					shell_data[row] = static_cast<int32_t>(shell - first_shell + 1);
					if (!WriteFace(*fields[3], model, f, polygon_data[row])) {
						FlatVector::SetNull(*fields[3], row, true);
					}
				}
			}
		}
		list_entries[i] = {offset, faces};
		offset += faces;
	}
	ListVector::SetListSize(result, offset);
	if (args.AllConstant()) {
		result.SetVectorType(VectorType::CONSTANT_VECTOR);
	}
}

static void ST_3DFaceNFun(DataChunk &args, ExpressionState &state, Vector &result) {
	duckdb_3d::SolidModel model;
	BinaryExecutor::ExecuteWithNulls<string_t, int64_t, string_t>(
	    args.data[0], args.data[1], result, args.size(), [&](string_t solid, int64_t n, ValidityMask &mask, idx_t idx) {
		    duckdb_3d::DeserializePayloadInto(reinterpret_cast<const uint8_t *>(solid.GetData()), solid.GetSize(),
		                                      model);
		    string_t out;
		    if (n < 1 || n > static_cast<int64_t>(model.FaceCount()) ||
		        !WriteFace(result, model, static_cast<uint32_t>(n - 1), out)) {
			    mask.SetInvalid(idx);
			    return string_t();
		    }
		    return out;
	    });
}

//! The per-face measures of a SOLID_3D or GEOM_3D payload.
static std::vector<duckdb_3d::FaceMeasure> FaceMeasures(string_t payload, duckdb_3d::SolidModel &solid) {
	auto data = reinterpret_cast<const uint8_t *>(payload.GetData());
	auto size = payload.GetSize();
	switch (GetPayloadKind(data, size)) {
	case PayloadKind::Solid:
		duckdb_3d::DeserializePayloadInto(data, size, solid);
		return duckdb_3d::SolidFaceMeasures(solid);
	case PayloadKind::Geom:
		return duckdb_3d::GeomFaceMeasures(duckdb_3d::DeserializeGeomPayload(data, size));
	default:
		throw InvalidInputException("argument is not a SOLID_3D or GEOM_3D value");
	}
}

//! Run `emit(face, child_vector, row)` for every face of every row, writing a
//! list per row; `emit` returns false to make that element NULL.
template <class EMIT>
static void PerFace(DataChunk &args, Vector &result, EMIT emit) {
	auto count = args.size();
	UnifiedVectorFormat input;
	args.data[0].ToUnifiedFormat(count, input);
	auto values = UnifiedVectorFormat::GetData<string_t>(input);
	auto list_entries = FlatVector::GetData<list_entry_t>(result);
	auto &child = ListVector::GetEntry(result);
	duckdb_3d::SolidModel solid;
	idx_t offset = 0;
	for (idx_t i = 0; i < count; i++) {
		auto idx = input.sel->get_index(i);
		if (!input.validity.RowIsValid(idx)) {
			FlatVector::SetNull(result, i, true);
			list_entries[i] = {offset, 0};
			continue;
		}
		auto faces = FaceMeasures(values[idx], solid);
		ListVector::Reserve(result, offset + faces.size());
		for (idx_t f = 0; f < faces.size(); f++) {
			if (!emit(faces[f], child, offset + f)) {
				FlatVector::SetNull(child, offset + f, true);
			}
		}
		list_entries[i] = {offset, faces.size()};
		offset += faces.size();
	}
	ListVector::SetListSize(result, offset);
	if (args.AllConstant()) {
		result.SetVectorType(VectorType::CONSTANT_VECTOR);
	}
}

static void ST_3DFaceAreasFun(DataChunk &args, ExpressionState &state, Vector &result) {
	PerFace(args, result, [](const duckdb_3d::FaceMeasure &face, Vector &child, idx_t row) {
		FlatVector::GetData<double>(child)[row] = face.area;
		return face.has_area;
	});
}

static void ST_3DFaceNormalsFun(DataChunk &args, ExpressionState &state, Vector &result) {
	PerFace(args, result, [](const duckdb_3d::FaceMeasure &face, Vector &child, idx_t row) {
		auto &xyz = StructVector::GetEntries(child);
		FlatVector::GetData<double>(*xyz[0])[row] = face.normal.x;
		FlatVector::GetData<double>(*xyz[1])[row] = face.normal.y;
		FlatVector::GetData<double>(*xyz[2])[row] = face.normal.z;
		return face.has_normal;
	});
}

static void ST_3DFaceSlopesFun(DataChunk &args, ExpressionState &state, Vector &result) {
	PerFace(args, result, [](const duckdb_3d::FaceMeasure &face, Vector &child, idx_t row) {
		if (!face.has_normal) {
			return false;
		}
		FlatVector::GetData<double>(child)[row] = duckdb_3d::FaceSlopeDegrees(face.normal);
		return true;
	});
}

static void ST_3DFaceAzimuthsFun(DataChunk &args, ExpressionState &state, Vector &result) {
	PerFace(args, result, [](const duckdb_3d::FaceMeasure &face, Vector &child, idx_t row) {
		return face.has_normal && duckdb_3d::FaceAzimuthDegrees(face.normal, FlatVector::GetData<double>(child)[row]);
	});
}

// ──────────────────────────────────────────────────────────────
// Shells and parts: ST_3DExteriorShell, ST_3DNumInteriorShells,
// ST_3DInteriorShellN, ST_3DGeometryN
// ──────────────────────────────────────────────────────────────
// As PostGIS's ST_ExteriorRing and ST_NumInteriorRings give NULL for a
// MultiPolygon, the shell functions give NULL for a collection of solids:
// ST_3DGeometryN picks one first.

static string_t WriteGeomModel(Vector &result, const duckdb_3d::GeomModel &model) {
	auto payload = duckdb_3d::SerializeGeomPayload(model);
	return StringVector::AddStringOrBlob(result,
	                                     string_t(reinterpret_cast<const char *>(payload.data()), payload.size()));
}

//! A shell as a GEOM_3D surface, or NULL when one of its faces admits no tiling
//! and so cannot be a GEOM_3D polygon, as ST_3DFaces gives that face.
static string_t WriteShell(Vector &result, const duckdb_3d::SolidModel &model, uint32_t shell, ValidityMask &mask,
                           idx_t idx) {
	try {
		return WriteGeomModel(result, duckdb_3d::SolidShellSurface(model, shell));
	} catch (std::invalid_argument &) {
		mask.SetInvalid(idx);
		return string_t();
	}
}

static void ST_3DExteriorShellFun(DataChunk &args, ExpressionState &state, Vector &result) {
	duckdb_3d::SolidModel model;
	UnaryExecutor::ExecuteWithNulls<string_t, string_t>(
	    args.data[0], result, args.size(), [&](string_t solid, ValidityMask &mask, idx_t idx) {
		    duckdb_3d::DeserializePayloadInto(reinterpret_cast<const uint8_t *>(solid.GetData()), solid.GetSize(),
		                                      model);
		    if (model.SolidCount() != 1) {
			    mask.SetInvalid(idx);
			    return string_t();
		    }
		    return WriteShell(result, model, 0, mask, idx);
	    });
}

static void ST_3DNumInteriorShellsFun(DataChunk &args, ExpressionState &state, Vector &result) {
	UnaryExecutor::ExecuteWithNulls<string_t, int32_t>(
	    args.data[0], result, args.size(), [&](string_t solid, ValidityMask &mask, idx_t idx) {
		    auto info =
		        duckdb_3d::ReadSolidPayloadHeader(reinterpret_cast<const uint8_t *>(solid.GetData()), solid.GetSize());
		    if (info.solid_count != 1) {
			    mask.SetInvalid(idx);
			    return int32_t(0);
		    }
		    return static_cast<int32_t>(info.shell_count - 1);
	    });
}

static void ST_3DInteriorShellNFun(DataChunk &args, ExpressionState &state, Vector &result) {
	duckdb_3d::SolidModel model;
	BinaryExecutor::ExecuteWithNulls<string_t, int64_t, string_t>(
	    args.data[0], args.data[1], result, args.size(), [&](string_t solid, int64_t n, ValidityMask &mask, idx_t idx) {
		    duckdb_3d::DeserializePayloadInto(reinterpret_cast<const uint8_t *>(solid.GetData()), solid.GetSize(),
		                                      model);
		    if (model.SolidCount() != 1 || n < 1 || n >= static_cast<int64_t>(model.ShellCount())) {
			    mask.SetInvalid(idx);
			    return string_t();
		    }
		    return WriteShell(result, model, static_cast<uint32_t>(n), mask, idx);
	    });
}

static void ST_3DGeometryNSolidFun(DataChunk &args, ExpressionState &state, Vector &result) {
	duckdb_3d::SolidModel model;
	BinaryExecutor::ExecuteWithNulls<string_t, int64_t, string_t>(
	    args.data[0], args.data[1], result, args.size(), [&](string_t solid, int64_t n, ValidityMask &mask, idx_t idx) {
		    duckdb_3d::DeserializePayloadInto(reinterpret_cast<const uint8_t *>(solid.GetData()), solid.GetSize(),
		                                      model);
		    if (n < 1 || n > static_cast<int64_t>(model.SolidCount())) {
			    mask.SetInvalid(idx);
			    return string_t();
		    }
		    auto payload = duckdb_3d::SerializePayload(duckdb_3d::SolidPart(model, static_cast<uint32_t>(n - 1)));
		    return StringVector::AddStringOrBlob(
		        result, string_t(reinterpret_cast<const char *>(payload.data()), payload.size()));
	    });
}

static void ST_3DGeometryNGeomFun(DataChunk &args, ExpressionState &state, Vector &result) {
	BinaryExecutor::ExecuteWithNulls<string_t, int64_t, string_t>(
	    args.data[0], args.data[1], result, args.size(), [&](string_t geom, int64_t n, ValidityMask &mask, idx_t idx) {
		    auto model =
		        duckdb_3d::DeserializeGeomPayload(reinterpret_cast<const uint8_t *>(geom.GetData()), geom.GetSize());
		    duckdb_3d::GeomModel element;
		    if (n < 1 || n > UINT32_MAX || !duckdb_3d::GeomElement(model, static_cast<uint32_t>(n - 1), element)) {
			    mask.SetInvalid(idx);
			    return string_t();
		    }
		    return WriteGeomModel(result, element);
	    });
}

void RegisterFaceFunctions(ExtensionLoader &loader, const LogicalType &solid_3d_type, const LogicalType &geom_3d_type) {
	child_list_t<LogicalType> face_fields;
	face_fields.push_back({"face", LogicalTypeId::INTEGER});
	face_fields.push_back({"solid", LogicalTypeId::INTEGER});
	face_fields.push_back({"shell", LogicalTypeId::INTEGER});
	face_fields.push_back({"polygon", geom_3d_type});
	auto faces_type = LogicalType::LIST(LogicalType::STRUCT(std::move(face_fields)));
	const string box =
	    "ST_3DExtrude(ST_Geom3DFromWKB('POLYGON Z ((0 0 0, 2 0 0, 2 2 0, 0 2 0, 0 0 0))'::GEOMETRY), 3.0)";

	RegisterDocumented(loader, ScalarFunction("st_3dfaces", {solid_3d_type}, faces_type, ST_3DFacesFun),
	                   {{"solid"},
	                    "Returns every face of a SOLID_3D as a list of STRUCT(face, solid, shell, polygon): the face "
	                    "number from 1 in storage order, the solid and the shell within it (1 is the exterior), and "
	                    "the face as a GEOM_3D Polygon in the solid's CRS. A face that cannot be triangulated has a "
	                    "NULL polygon. UNNEST it for one row per face.",
	                    "UNNEST(ST_3DFaces(" + box + "))",
	                    {"faces"}});

	ScalarFunctionSet face_n("st_3dfacen");
	face_n.AddFunction(ScalarFunction({solid_3d_type, LogicalType::BIGINT}, geom_3d_type, ST_3DFaceNFun));
	RegisterDocumented(loader, std::move(face_n),
	                   {{"solid", "n"},
	                    "Returns face n of a SOLID_3D, numbered from 1 as ST_3DFaces numbers them, as a GEOM_3D "
	                    "Polygon in the solid's CRS; NULL when n is out of range or the face cannot be triangulated.",
	                    "ST_3DFaceN(" + box + ", 1)",
	                    {"faces"}});

	RegisterDocumented(loader,
	                   ScalarFunction("st_3dexteriorshell", {solid_3d_type}, geom_3d_type, ST_3DExteriorShellFun),
	                   {{"solid"},
	                    "Returns the exterior shell of a SOLID_3D holding one solid as a GEOM_3D PolyhedralSurface, "
	                    "one patch per face, in the solid's CRS; NULL for a collection of solids (take one with "
	                    "ST_3DGeometryN).",
	                    "ST_3DExteriorShell(" + box + ")",
	                    {"shells"}});
	RegisterDocumented(
	    loader,
	    ScalarFunction("st_3dnuminteriorshells", {solid_3d_type}, LogicalType::INTEGER, ST_3DNumInteriorShellsFun),
	    {{"solid"},
	     "Returns the number of interior shells (cavities) of a SOLID_3D holding one solid; NULL for a "
	     "collection of solids. Read from the header.",
	     "ST_3DNumInteriorShells(" + box + ")",
	     {"shells"}});
	RegisterDocumented(loader,
	                   ScalarFunction("st_3dinteriorshelln", {solid_3d_type, LogicalType::BIGINT}, geom_3d_type,
	                                  ST_3DInteriorShellNFun),
	                   {{"solid", "n"},
	                    "Returns interior shell n (from 1) of a SOLID_3D holding one solid as a GEOM_3D "
	                    "PolyhedralSurface in the solid's CRS, its faces wound into the cavity; NULL when n is out of "
	                    "range or for a collection of solids.",
	                    "ST_3DInteriorShellN(" + box + ", 1)",
	                    {"shells"}});
	ScalarFunctionSet geometry_n("st_3dgeometryn");
	geometry_n.AddFunction(ScalarFunction({solid_3d_type, LogicalType::BIGINT}, solid_3d_type, ST_3DGeometryNSolidFun));
	geometry_n.AddFunction(ScalarFunction({geom_3d_type, LogicalType::BIGINT}, geom_3d_type, ST_3DGeometryNGeomFun));
	RegisterDocumented(loader, std::move(geometry_n),
	                   {{"geom", "n"},
	                    "Returns element n (from 1), as PostGIS's ST_GeometryN: of a SOLID_3D its n-th solid, "
	                    "validated on its own; of a GEOM_3D the n-th part of a Multi geometry, the n-th patch of a "
	                    "PolyhedralSurface, or the value itself for n = 1 of a single geometry. NULL when n is out of "
	                    "range. The CRS is kept.",
	                    "ST_3DGeometryN(ST_Geom3DFromWKB('MULTIPOINT Z (1 2 3, 4 5 6)'::GEOMETRY), 2)",
	                    {"shells"}});

	struct PerFaceFunction {
		const char *name;
		const char *display;
		LogicalType element;
		scalar_function_t function;
		const char *description;
	};
	child_list_t<LogicalType> xyz;
	xyz.push_back({"x", LogicalTypeId::DOUBLE});
	xyz.push_back({"y", LogicalTypeId::DOUBLE});
	xyz.push_back({"z", LogicalTypeId::DOUBLE});
	const PerFaceFunction per_face[] = {
	    {"st_3dfaceareas", "ST_3DFaceAreas", LogicalTypeId::DOUBLE, ST_3DFaceAreasFun,
	     "Returns the area of each face of a SOLID_3D, or each polygon of a GEOM_3D, holes excluded, in face "
	     "order. They sum to ST_3DSurfaceArea or ST_3DArea. A face that cannot be triangulated is NULL."},
	    {"st_3dfacenormals", "ST_3DFaceNormals", LogicalType::STRUCT(std::move(xyz)), ST_3DFaceNormalsFun,
	     "Returns the unit normal of each face of a SOLID_3D, or each polygon of a GEOM_3D, as STRUCT(x, y, z), "
	     "following the right-hand rule around the exterior ring: out of a valid solid, into a cavity. NULL for a "
	     "face without area."},
	    {"st_3dfaceslopes", "ST_3DFaceSlopes", LogicalTypeId::DOUBLE, ST_3DFaceSlopesFun,
	     "Returns the slope of each face of a SOLID_3D, or each polygon of a GEOM_3D: its tilt from horizontal in "
	     "degrees, 0 for horizontal and 90 for vertical, whichever way it faces. NULL for a face without area."},
	    {"st_3dfaceazimuths", "ST_3DFaceAzimuths", LogicalTypeId::DOUBLE, ST_3DFaceAzimuthsFun,
	     "Returns the azimuth of each face of a SOLID_3D, or each polygon of a GEOM_3D: the compass direction its "
	     "normal leans toward, in degrees clockwise from +Y (grid north), in [0, 360). NULL for a horizontal face "
	     "and a face without area."},
	};
	for (auto &f : per_face) {
		ScalarFunctionSet set(f.name);
		set.AddFunction(ScalarFunction({solid_3d_type}, LogicalType::LIST(f.element), f.function));
		set.AddFunction(ScalarFunction({geom_3d_type}, LogicalType::LIST(f.element), f.function));
		RegisterDocumented(loader, std::move(set),
		                   {{"geom"}, f.description, string(f.display) + "(" + box + ")", {"faces"}});
	}
}

} // namespace duckdb
