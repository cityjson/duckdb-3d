#include "functions/three_d_functions.hpp"

#include "duckdb/function/scalar_function.hpp"

#include "kernel/core_types.hpp"
#include "kernel/geom_distance.hpp"
#include "kernel/geom_model.hpp"

#include <cstdint>

namespace duckdb {

// Kernel names this file uses unqualified. Using-declarations rather than a
// using-directive, which clang-tidy's google-build-using-namespace rejects.
using duckdb_3d::DeserializeGeomPayload;
using duckdb_3d::GeomModel;
using duckdb_3d::GeomType;
using duckdb_3d::ReadGeomPayloadHeader;

// ST_3DDistance(g1 GEOM_3D, g2 GEOM_3D) → DOUBLE
static double Geom3DDistanceSQL(string_t g1, string_t g2) {
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
	auto d1 = reinterpret_cast<const uint8_t *>(g1.GetData());
	auto d2 = reinterpret_cast<const uint8_t *>(g2.GetData());
	// The bbox lives in the fixed front header: reject far-apart pairs in O(1)
	// without materialising either geometry body. This is a pure fast *negative*
	// — the bbox gap is a lower bound on the true distance, so exceeding `dist`
	// is conclusive. Anything not rejected here still takes the full path.
	auto h1 = ReadGeomPayloadHeader(d1, g1.GetSize());
	auto h2 = ReadGeomPayloadHeader(d2, g2.GetSize());
	if (BBoxDistance(h1.bbox, h2.bbox) > dist) {
		return false;
	}
	auto m1 = DeserializeGeomPayload(d1, g1.GetSize());
	auto m2 = DeserializeGeomPayload(d2, g2.GetSize());
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

// ST_3DMaxDistance(g1 GEOM_3D, g2 GEOM_3D) → DOUBLE
static double Geom3DMaxDistanceSQL(string_t g1, string_t g2) {
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
	BinaryExecutor::Execute<string_t, string_t, bool>(
	    args.data[0], args.data[1], result, args.size(),
	    [&](string_t g1, string_t g2) { return Geom3DWithinSQL(g1, g2, duckdb_3d::kEpsIntersect); });
}

// ST_3DClosestPoint(g1 GEOM_3D, g2 GEOM_3D) → GEOM_3D (Point)
// Returns the point on g1 that is closest to g2.
static void ST_3DClosestPointFun(DataChunk &args, ExpressionState &state, Vector &result) {
	BinaryExecutor::Execute<string_t, string_t, string_t>(
	    args.data[0], args.data[1], result, args.size(), [&](string_t g1, string_t g2) {
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

void RegisterDistanceFunctions(ExtensionLoader &loader, const LogicalType &geom_3d_type) {
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
}

} // namespace duckdb
