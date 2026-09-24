#include "functions/three_d_functions.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/types/geometry.hpp"
#include "duckdb/function/function_set.hpp"
#include "duckdb/function/scalar_function.hpp"

#include "kernel/geom_analysis.hpp"
#include "kernel/geom_construct.hpp"
#include "kernel/geom_model.hpp"
#include "kernel/geom_serialize.hpp"
#include "kernel/geom_wkb_parser.hpp"

#include <cmath>
#include <cstdint>

namespace duckdb {

// Kernel names this file uses unqualified. Using-declarations rather than a
// using-directive, which clang-tidy's google-build-using-namespace rejects.
using duckdb_3d::DeserializeGeomPayload;
using duckdb_3d::GeomModel;
using duckdb_3d::GeomType;
using duckdb_3d::ParseGeomWKB;
using duckdb_3d::ReadGeomPayloadHeader;

// ──────────────────────────────────────────────────────────────
// GEOM_3D: general geometry construction and accessors
// ──────────────────────────────────────────────────────────────

// ST_Geom3DFromWKB(wkb BLOB | GEOMETRY) → GEOM_3D
// (named to avoid clashing with DuckDB core's st_geomfromwkb -> GEOMETRY)
static void Geom3DFromWKBVector(Vector &wkb_vec, idx_t count, Vector &result) {
	UnaryExecutor::Execute<string_t, string_t>(wkb_vec, result, count, [&](string_t wkb) {
		auto model = ParseGeomWKB(reinterpret_cast<const uint8_t *>(wkb.GetData()), wkb.GetSize());
		auto payload = SerializeGeomPayload(model);
		return StringVector::AddStringOrBlob(result,
		                                     string_t(reinterpret_cast<const char *>(payload.data()), payload.size()));
	});
}

static void ST_Geom3DFromWKBFun(DataChunk &args, ExpressionState &state, Vector &result) {
	Geom3DFromWKBVector(args.data[0], args.size(), result);
}

// ST_Geom3DFromWKB(geom GEOMETRY) → GEOM_3D. A CityParquet footprint column
// carries the Parquet GEOMETRY logical type, so DuckDB hands it over as GEOMETRY
// rather than BLOB. Geometry::ToBinary is the supported way down to WKB — the
// storage is physically a string, but not contractually raw WKB, so the string_t
// must not simply be reinterpreted.
static void ST_Geom3DFromGeometryFun(DataChunk &args, ExpressionState &state, Vector &result) {
	Vector wkb_vec(LogicalType::BLOB);
	Geometry::ToBinary(args.data[0], wkb_vec, args.size());
	Geom3DFromWKBVector(wkb_vec, args.size(), result);
}

static unique_ptr<FunctionData> BindGeom3DFromWkbArg(ClientContext &, ScalarFunction &bound_function,
                                                     vector<unique_ptr<Expression>> &arguments) {
	return BindWkbArgument(bound_function, arguments, ST_Geom3DFromWKBFun, ST_Geom3DFromGeometryFun);
}

//! Reject a type code that is not in the GEOM_3D enum. The header-read accessors
//! below consult exactly this field, so a crafted code must not be laundered into
//! a plausible generic answer — unlike ST_3DZMin/ST_3DZMax, whose contract is
//! bbox-only and never looks at the code. The codes are non-contiguous (1-7, 15),
//! hence an explicit whitelist rather than a range check.
static duckdb_3d::GeomType ValidatedGeomType(duckdb_3d::GeomType type) {
	switch (type) {
	case GeomType::Point:
	case GeomType::LineString:
	case GeomType::Polygon:
	case GeomType::MultiPoint:
	case GeomType::MultiLineString:
	case GeomType::MultiPolygon:
	case GeomType::GeometryCollection:
	case GeomType::PolyhedralSurface:
		return type;
	default:
		throw InvalidInputException("GEOM_3D payload: unknown geometry type code");
	}
}

static const char *GeomTypeName(duckdb_3d::GeomType type) {
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
static void ST_3DGeometryTypeFun(DataChunk &args, ExpressionState &state, Vector &result) {
	UnaryExecutor::Execute<string_t, string_t>(args.data[0], result, args.size(), [&](string_t geom) {
		// Only the type code is needed — read the O(1) front header. Magic, major
		// version and the type code itself are still checked; a corrupt *body* is
		// not, since it is never touched.
		auto info = ReadGeomPayloadHeader(reinterpret_cast<const uint8_t *>(geom.GetData()), geom.GetSize());
		return StringVector::AddString(result, GeomTypeName(ValidatedGeomType(info.type)));
	});
}

// Point ordinate accessors: ST_3DX / ST_3DY / ST_3DZ(point GEOM_3D) → DOUBLE.
enum class Ordinate { X, Y, Z };

static double PointOrdinate(string_t geom, Ordinate ord) {
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

static void ST_3DXFun(DataChunk &args, ExpressionState &state, Vector &result) {
	UnaryExecutor::Execute<string_t, double>(args.data[0], result, args.size(),
	                                         [](string_t geom) { return PointOrdinate(geom, Ordinate::X); });
}
static void ST_3DYFun(DataChunk &args, ExpressionState &state, Vector &result) {
	UnaryExecutor::Execute<string_t, double>(args.data[0], result, args.size(),
	                                         [](string_t geom) { return PointOrdinate(geom, Ordinate::Y); });
}
static void ST_3DZFun(DataChunk &args, ExpressionState &state, Vector &result) {
	UnaryExecutor::Execute<string_t, double>(args.data[0], result, args.size(),
	                                         [](string_t geom) { return PointOrdinate(geom, Ordinate::Z); });
}

static double Geom3DLength(string_t geom) {
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

static void ST_CoordDimFun(DataChunk &args, ExpressionState &state, Vector &result) {
	UnaryExecutor::Execute<string_t, int32_t>(args.data[0], result, args.size(),
	                                          [](string_t geom) { return CoordinateDimension3D(); });
}

// ST_3DExtrude(polygon GEOM_3D, height DOUBLE) → SOLID_3D
static void ST_3DExtrudeFun(DataChunk &args, ExpressionState &state, Vector &result) {
	BinaryExecutor::Execute<string_t, double, string_t>(
	    args.data[0], args.data[1], result, args.size(), [&](string_t geom, double height) {
		    auto poly = DeserializeGeomPayload(reinterpret_cast<const uint8_t *>(geom.GetData()), geom.GetSize());
		    auto solid = BuildExtrudedSolid(poly, height);
		    auto payload = SerializePayload(solid);
		    return StringVector::AddStringOrBlob(
		        result, string_t(reinterpret_cast<const char *>(payload.data()), payload.size()));
	    });
}

// ST_MakeSolid(surface GEOM_3D) → SOLID_3D
static void ST_MakeSolidFun(DataChunk &args, ExpressionState &state, Vector &result) {
	UnaryExecutor::Execute<string_t, string_t>(args.data[0], result, args.size(), [&](string_t geom) {
		auto surface = DeserializeGeomPayload(reinterpret_cast<const uint8_t *>(geom.GetData()), geom.GetSize());
		auto solid = BuildSolidFromSurface(surface);
		auto payload = SerializePayload(solid);
		return StringVector::AddStringOrBlob(result,
		                                     string_t(reinterpret_cast<const char *>(payload.data()), payload.size()));
	});
}

// ST_3DAsText(geom GEOM_3D) → VARCHAR
static void ST_3DAsTextFun(DataChunk &args, ExpressionState &state, Vector &result) {
	UnaryExecutor::Execute<string_t, string_t>(args.data[0], result, args.size(), [&](string_t geom) {
		auto model = DeserializeGeomPayload(reinterpret_cast<const uint8_t *>(geom.GetData()), geom.GetSize());
		auto text = Geom3DAsText(model);
		return StringVector::AddString(result, text);
	});
}

// ST_3DAsGeoJSON(geom GEOM_3D) → VARCHAR
static void ST_3DAsGeoJSONFun(DataChunk &args, ExpressionState &state, Vector &result) {
	UnaryExecutor::Execute<string_t, string_t>(args.data[0], result, args.size(), [&](string_t geom) {
		auto model = DeserializeGeomPayload(reinterpret_cast<const uint8_t *>(geom.GetData()), geom.GetSize());
		auto text = Geom3DAsGeoJSON(model);
		return StringVector::AddString(result, text);
	});
}

// ST_3DAsBinary(geom GEOM_3D) → BLOB
static void ST_3DAsBinaryFun(DataChunk &args, ExpressionState &state, Vector &result) {
	UnaryExecutor::Execute<string_t, string_t>(args.data[0], result, args.size(), [&](string_t geom) {
		auto model = DeserializeGeomPayload(reinterpret_cast<const uint8_t *>(geom.GetData()), geom.GetSize());
		auto binary = Geom3DAsBinary(model);
		return StringVector::AddStringOrBlob(result,
		                                     string_t(reinterpret_cast<const char *>(binary.data()), binary.size()));
	});
}

// ST_IsPlanar(geom GEOM_3D) → BOOLEAN
static void ST_IsPlanarFun(DataChunk &args, ExpressionState &state, Vector &result) {
	UnaryExecutor::Execute<string_t, bool>(args.data[0], result, args.size(), [](string_t geom) {
		auto model = DeserializeGeomPayload(reinterpret_cast<const uint8_t *>(geom.GetData()), geom.GetSize());
		return Geom3DIsPlanar(model);
	});
}

// ST_3DCentroid(geom GEOM_3D) → GEOM_3D (Point)
static void ST_3DCentroidFun(DataChunk &args, ExpressionState &state, Vector &result) {
	UnaryExecutor::Execute<string_t, string_t>(args.data[0], result, args.size(), [&](string_t geom) {
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
		auto model = DeserializeGeomPayload(reinterpret_cast<const uint8_t *>(geom.GetData()), geom.GetSize());
		auto payload = SerializeGeomPayload(model);
		return StringVector::AddStringOrBlob(result,
		                                     string_t(reinterpret_cast<const char *>(payload.data()), payload.size()));
	});
}

// ST_3DConvexHull(geom GEOM_3D) → GEOM_3D
// 2D monotone-chain hull over XY-projected vertices; output Z = input min Z.
static void ST_3DConvexHullFun(DataChunk &args, ExpressionState &state, Vector &result) {
	UnaryExecutor::Execute<string_t, string_t>(args.data[0], result, args.size(), [&](string_t geom) {
		auto model = DeserializeGeomPayload(reinterpret_cast<const uint8_t *>(geom.GetData()), geom.GetSize());
		auto hull = Geom3DConvexHull(model);
		auto payload = SerializeGeomPayload(hull);
		return StringVector::AddStringOrBlob(result,
		                                     string_t(reinterpret_cast<const char *>(payload.data()), payload.size()));
	});
}

// ST_3DDimension(geom GEOM_3D) → INTEGER
static int32_t GeomDimension(duckdb_3d::GeomType type) {
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

static void ST_3DDimensionFun(DataChunk &args, ExpressionState &state, Vector &result) {
	UnaryExecutor::Execute<string_t, int32_t>(args.data[0], result, args.size(), [](string_t geom) {
		// Only the type code is needed — read the O(1) front header (see the
		// header-only contract noted on ST_3DGeometryTypeFun).
		auto info = ReadGeomPayloadHeader(reinterpret_cast<const uint8_t *>(geom.GetData()), geom.GetSize());
		return GeomDimension(ValidatedGeomType(info.type));
	});
}

// ST_3DNumGeometries(geom GEOM_3D) → INTEGER
static int32_t GeomNumGeometries(const duckdb_3d::GeomModel &model) {
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

static void ST_3DNumGeometriesFun(DataChunk &args, ExpressionState &state, Vector &result) {
	UnaryExecutor::Execute<string_t, int32_t>(args.data[0], result, args.size(), [](string_t geom) {
		auto model = DeserializeGeomPayload(reinterpret_cast<const uint8_t *>(geom.GetData()), geom.GetSize());
		return GeomNumGeometries(model);
	});
}

void RegisterGeomAccessorFunctions(ExtensionLoader &loader, const LogicalType &solid_3d_type,
                                   const LogicalType &geom_3d_type) {
	// GEOM_3D construction and accessors. One ANY candidate, dispatched at bind
	// time, so that BLOB, GEOMETRY and an untyped NULL all bind (see BindWkbArgument).
	RegisterDocumented(
	    loader,
	    ScalarFunction("st_geom3dfromwkb", {LogicalType::ANY}, geom_3d_type, ST_Geom3DFromWKBFun, BindGeom3DFromWkbArg),
	    {{"wkb"},
	     "Builds a GEOM_3D from WKB or GEOMETRY: Point, LineString, Polygon, their Multi forms, or PolyhedralSurface, "
	     "all with Z. Raises on anything else, GeometryCollection included.",
	     "ST_Geom3DFromWKB('POINT Z (1 2 3)'::GEOMETRY)",
	     {"import"}});

	RegisterDocumented(
	    loader, ScalarFunction("st_3dgeometrytype", {geom_3d_type}, LogicalType::VARCHAR, ST_3DGeometryTypeFun),
	    {{"geom"},
	     "Returns the geometry class of a GEOM_3D value, such as 'ST_Point' or 'ST_PolyhedralSurface'.",
	     "ST_3DGeometryType(ST_Geom3DFromWKB('POLYGON Z ((0 0 0, 2 0 0, 2 2 0, 0 2 0, 0 0 0))'::GEOMETRY))",
	     {"introspection"}});
	RegisterDocumented(loader, ScalarFunction("st_3dx", {geom_3d_type}, LogicalType::DOUBLE, ST_3DXFun),
	                   {{"geom"},
	                    "Returns the X coordinate of a GEOM_3D Point. Raises for any other geometry class.",
	                    "ST_3DX(ST_Geom3DFromWKB('POINT Z (1 2 3)'::GEOMETRY))",
	                    {"introspection"}});
	RegisterDocumented(loader, ScalarFunction("st_3dy", {geom_3d_type}, LogicalType::DOUBLE, ST_3DYFun),
	                   {{"geom"},
	                    "Returns the Y coordinate of a GEOM_3D Point. Raises for any other geometry class.",
	                    "ST_3DY(ST_Geom3DFromWKB('POINT Z (1 2 3)'::GEOMETRY))",
	                    {"introspection"}});
	RegisterDocumented(loader, ScalarFunction("st_3dz", {geom_3d_type}, LogicalType::DOUBLE, ST_3DZFun),
	                   {{"geom"},
	                    "Returns the Z coordinate of a GEOM_3D Point. Raises for any other geometry class.",
	                    "ST_3DZ(ST_Geom3DFromWKB('POINT Z (1 2 3)'::GEOMETRY))",
	                    {"introspection"}});
	RegisterDocumented(loader, ScalarFunction("st_coorddim", {geom_3d_type}, LogicalType::INTEGER, ST_CoordDimFun),
	                   {{"geom"},
	                    "Returns the coordinate dimension of a GEOM_3D value, always 3.",
	                    "ST_CoordDim(ST_Geom3DFromWKB('POINT Z (1 2 3)'::GEOMETRY))",
	                    {"introspection"}});
	RegisterDocumented(
	    loader, ScalarFunction("st_3ddimension", {geom_3d_type}, LogicalType::INTEGER, ST_3DDimensionFun),
	    {{"geom"},
	     "Returns the topological dimension of a GEOM_3D value: 0 for points, 1 for lines, 2 for surfaces.",
	     "ST_3DDimension(ST_Geom3DFromWKB('POLYGON Z ((0 0 0, 2 0 0, 2 2 0, 0 2 0, 0 0 0))'::GEOMETRY))",
	     {"introspection"}});
	RegisterDocumented(
	    loader, ScalarFunction("st_3dnumgeometries", {geom_3d_type}, LogicalType::INTEGER, ST_3DNumGeometriesFun),
	    {{"geom"},
	     "Returns the number of member geometries of a GEOM_3D value: the member count of a Multi geometry, 1 "
	     "otherwise.",
	     "ST_3DNumGeometries(ST_Geom3DFromWKB('POLYGON Z ((0 0 0, 2 0 0, 2 2 0, 0 2 0, 0 0 0))'::GEOMETRY))",
	     {"introspection"}});
	RegisterDocumented(loader, ScalarFunction("st_3dlength", {geom_3d_type}, LogicalType::DOUBLE, ST_3DLengthFun),
	                   {{"geom"},
	                    "Returns the 3D length of a GEOM_3D LineString or MultiLineString in input units, and 0 for "
	                    "any other geometry class.",
	                    "ST_3DLength(ST_Geom3DFromWKB('LINESTRING Z (0 0 0, 3 4 12)'::GEOMETRY))",
	                    {"measurement"}});
	RegisterDocumented(loader, ScalarFunction("st_3dastext", {geom_3d_type}, LogicalType::VARCHAR, ST_3DAsTextFun),
	                   {{"geom"},
	                    "Returns a GEOM_3D value as ISO WKT with Z; ordinates are formatted with 9 significant digits.",
	                    "ST_3DAsText(ST_Geom3DFromWKB('POINT Z (1 2 3)'::GEOMETRY))",
	                    {"export"}});
	RegisterDocumented(
	    loader, ScalarFunction("st_3dasgeojson", {geom_3d_type}, LogicalType::VARCHAR, ST_3DAsGeoJSONFun),
	    {{"geom"},
	     "Returns a GEOM_3D value as a GeoJSON geometry; a PolyhedralSurface is emitted as a MultiPolygon.",
	     "ST_3DAsGeoJSON(ST_Geom3DFromWKB('POINT Z (1 2 3)'::GEOMETRY))",
	     {"export"}});
	RegisterDocumented(loader, ScalarFunction("st_3dasbinary", {geom_3d_type}, LogicalType::BLOB, ST_3DAsBinaryFun),
	                   {{"geom"},
	                    "Returns a GEOM_3D value as little-endian OGC/ISO WKB with Z.",
	                    "ST_3DAsBinary(ST_Geom3DFromWKB('POINT Z (1 2 3)'::GEOMETRY))",
	                    {"export"}});
	RegisterDocumented(
	    loader, ScalarFunction("st_isplanar", {geom_3d_type}, LogicalType::BOOLEAN, ST_IsPlanarFun),
	    {{"geom"},
	     "Returns whether each ring of a GEOM_3D surface is planar within tolerance; for points and lines, whether all "
	     "vertices are coplanar.",
	     "ST_IsPlanar(ST_Geom3DFromWKB('POLYGON Z ((0 0 0, 2 0 0, 2 2 0, 0 2 0, 0 0 0))'::GEOMETRY))",
	     {"introspection"}});
	RegisterDocumented(loader, ScalarFunction("st_3dcentroid", {geom_3d_type}, geom_3d_type, ST_3DCentroidFun),
	                   {{"geom"},
	                    "Returns the centroid of a GEOM_3D value as a Point Z: area-weighted for surfaces, "
	                    "length-weighted for lines, the vertex average for points.",
	                    "ST_3DCentroid(ST_Geom3DFromWKB('POLYGON Z ((0 0 0, 2 0 0, 2 2 0, 0 2 0, 0 0 0))'::GEOMETRY))",
	                    {"transform"}});
	RegisterDocumented(
	    loader, ScalarFunction("st_force3d", {geom_3d_type}, geom_3d_type, ST_Force3DFun),
	    {{"geom"},
	     "Returns a GEOM_3D value with XYZ coordinates; GEOM_3D already stores XYZ, so this is an identity.",
	     "ST_Force3D(ST_Geom3DFromWKB('POINT Z (1 2 3)'::GEOMETRY))",
	     {"transform"}});
	RegisterDocumented(loader, ScalarFunction("st_3dconvexhull", {geom_3d_type}, geom_3d_type, ST_3DConvexHullFun),
	                   {{"geom"},
	                    "Returns the 2D convex hull of a GEOM_3D value's XY projection as a Polygon Z at the input's "
	                    "minimum Z; degenerate inputs give a LineString Z or Point Z.",
	                    "ST_3DConvexHull(ST_Geom3DFromWKB('LINESTRING Z (0 0 0, 3 4 12)'::GEOMETRY))",
	                    {"transform"}});
	// Solid-producing constructors: return the SOLID_3D alias (like st_3dfromwkb) so
	// the typed measurement/introspection overloads compose directly on the result.
	RegisterDocumented(
	    loader, ScalarFunction("st_3dextrude", {geom_3d_type, LogicalType::DOUBLE}, solid_3d_type, ST_3DExtrudeFun),
	    {{"geom", "height"},
	     "Extrudes a GEOM_3D Polygon vertically by height into a closed, valid SOLID_3D prism. Raises unless the input "
	     "is a polygon with at least 3 distinct exterior vertices and height > 0.",
	     "ST_3DExtrude(ST_Geom3DFromWKB('POLYGON Z ((0 0 0, 2 0 0, 2 2 0, 0 2 0, 0 0 0))'::GEOMETRY), 3.0)",
	     {"transform"}});
	RegisterDocumented(loader, ScalarFunction("st_makesolid", {geom_3d_type}, solid_3d_type, ST_MakeSolidFun),
	                   {{"geom"},
	                    "Promotes a closed PolyhedralSurface GEOM_3D to a SOLID_3D. Raises unless the surface is "
	                    "already closed, manifold and oriented; nothing is repaired.",
	                    "ST_MakeSolid(ST_Geom3DFromWKB(ST_3DAsWKB(ST_3DExtrude(ST_Geom3DFromWKB('POLYGON Z ((0 0 0, 2 "
	                    "0 0, 2 2 0, 0 2 0, 0 0 0))'::GEOMETRY), 3.0))))",
	                    {"transform"}});
}

} // namespace duckdb
