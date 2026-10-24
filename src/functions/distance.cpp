#include "functions/three_d_functions.hpp"

#include "duckdb/function/scalar_function.hpp"

#include "kernel/core_types.hpp"
#include "kernel/geom_distance.hpp"
#include "kernel/geom_elements.hpp"
#include "kernel/geom_intersects.hpp"
#include "kernel/geom_model.hpp"
#include "kernel/payload.hpp"
#include "kernel/solid_relations.hpp"
#include "kernel/triangulation.hpp"

#include <cstdint>

namespace duckdb {

// Kernel names this file uses unqualified. Using-declarations rather than a
// using-directive, which clang-tidy's google-build-using-namespace rejects.
using duckdb_3d::BBox3D;
using duckdb_3d::DeserializeGeomPayload;
using duckdb_3d::GeomModel;
using duckdb_3d::GeomType;
using duckdb_3d::ReadGeomPayloadHeader;
using duckdb_3d::SolidModel;

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
// Exact: whether the two closed geometries share a point (touching counts).
static void ST_3DIntersectsFun(DataChunk &args, ExpressionState &state, Vector &result) {
	BinaryExecutor::Execute<string_t, string_t, bool>(
	    args.data[0], args.data[1], result, args.size(), [&](string_t g1, string_t g2) {
		    auto d1 = reinterpret_cast<const uint8_t *>(g1.GetData());
		    auto d2 = reinterpret_cast<const uint8_t *>(g2.GetData());
		    if (!duckdb_3d::BoxesOverlap(ReadGeomPayloadHeader(d1, g1.GetSize()).bbox,
		                                 ReadGeomPayloadHeader(d2, g2.GetSize()).bbox)) {
			    return false;
		    }
		    return Geom3DIntersects(DeserializeGeomPayload(d1, g1.GetSize()), DeserializeGeomPayload(d2, g2.GetSize()));
	    });
}

// ──────────────────────────────────────────────────────────────
// SOLID_3D overloads: a solid is the volume its shells bound.
// ──────────────────────────────────────────────────────────────

//! A solid argument's box, after refusing an invalid solid: it bounds no volume.
//! Read from the header, so a far-apart pair is settled without the body, and an
//! invalid solid raises whether or not it is near.
static BBox3D ValidSolidBox(string_t solid) {
	auto info = duckdb_3d::ReadSolidPayloadHeader(reinterpret_cast<const uint8_t *>(solid.GetData()), solid.GetSize());
	if (!info.validation.is_valid) {
		throw InvalidInputException("the solid is not valid (ST_3DValidationReport says why), so it bounds no volume");
	}
	return info.bbox;
}

static BBox3D GeomBox(string_t geom) {
	return ReadGeomPayloadHeader(reinterpret_cast<const uint8_t *>(geom.GetData()), geom.GetSize()).bbox;
}

//! Read a solid argument into `model`, reusing its arrays from row to row.
static const SolidModel &ReadSolid(string_t solid, SolidModel &model) {
	duckdb_3d::DeserializePayloadInto(reinterpret_cast<const uint8_t *>(solid.GetData()), solid.GetSize(), model);
	if (model.TriangleCount() == 0) {
		duckdb_3d::TriangulateSolidModel(model);
	}
	return model;
}

static GeomModel ReadGeom(string_t geom) {
	return DeserializeGeomPayload(reinterpret_cast<const uint8_t *>(geom.GetData()), geom.GetSize());
}

//! Apply `op(solid, geom)` to a (SOLID_3D, GEOM_3D) or, with SOLID_FIRST false,
//! a (GEOM_3D, SOLID_3D) pair: every relation here is symmetric.
template <bool SOLID_FIRST, class RESULT, class OP>
static void SolidGeomExecute(DataChunk &args, Vector &result, OP op) {
	SolidModel model;
	BinaryExecutor::Execute<string_t, string_t, RESULT>(args.data[0], args.data[1], result, args.size(),
	                                                    [&](string_t a, string_t b) {
		                                                    string_t solid = SOLID_FIRST ? a : b;
		                                                    string_t geom = SOLID_FIRST ? b : a;
		                                                    return op(solid, geom, model);
	                                                    });
}

template <class RESULT, class OP>
static void SolidSolidExecute(DataChunk &args, Vector &result, OP op) {
	SolidModel m1, m2;
	BinaryExecutor::Execute<string_t, string_t, RESULT>(args.data[0], args.data[1], result, args.size(),
	                                                    [&](string_t a, string_t b) { return op(a, b, m1, m2); });
}

static double SolidGeomDistance(string_t solid, string_t geom, SolidModel &model) {
	ValidSolidBox(solid);
	return duckdb_3d::Solid3DDistance(ReadSolid(solid, model), ReadGeom(geom));
}

static bool SolidGeomIntersects(string_t solid, string_t geom, SolidModel &model) {
	if (!duckdb_3d::BoxesOverlap(ValidSolidBox(solid), GeomBox(geom))) {
		return false;
	}
	return duckdb_3d::Solid3DIntersects(ReadSolid(solid, model), ReadGeom(geom));
}

static bool SolidGeomWithin(string_t solid, string_t geom, double dist, SolidModel &model) {
	if (duckdb_3d::BBoxDistance(ValidSolidBox(solid), GeomBox(geom)) > dist) {
		return false;
	}
	return duckdb_3d::Solid3DWithin(ReadSolid(solid, model), ReadGeom(geom), dist);
}

template <bool SOLID_FIRST>
static void ST_3DDistanceSolidGeomFun(DataChunk &args, ExpressionState &state, Vector &result) {
	SolidGeomExecute<SOLID_FIRST, double>(args, result, SolidGeomDistance);
}

template <bool SOLID_FIRST>
static void ST_3DIntersectsSolidGeomFun(DataChunk &args, ExpressionState &state, Vector &result) {
	SolidGeomExecute<SOLID_FIRST, bool>(args, result, SolidGeomIntersects);
}

template <bool SOLID_FIRST>
static void ST_3DDWithinSolidGeomFun(DataChunk &args, ExpressionState &state, Vector &result) {
	SolidModel model;
	TernaryExecutor::Execute<string_t, string_t, double, bool>(args.data[0], args.data[1], args.data[2], result,
	                                                           args.size(), [&](string_t a, string_t b, double dist) {
		                                                           string_t solid = SOLID_FIRST ? a : b;
		                                                           string_t geom = SOLID_FIRST ? b : a;
		                                                           if (dist < 0.0) {
			                                                           ValidSolidBox(solid);
			                                                           return false;
		                                                           }
		                                                           return SolidGeomWithin(solid, geom, dist, model);
	                                                           });
}

static void ST_3DDistanceSolidSolidFun(DataChunk &args, ExpressionState &state, Vector &result) {
	SolidSolidExecute<double>(args, result, [](string_t a, string_t b, SolidModel &m1, SolidModel &m2) {
		ValidSolidBox(a);
		ValidSolidBox(b);
		return duckdb_3d::Solid3DDistance(ReadSolid(a, m1), ReadSolid(b, m2));
	});
}

static void ST_3DIntersectsSolidSolidFun(DataChunk &args, ExpressionState &state, Vector &result) {
	SolidSolidExecute<bool>(args, result, [](string_t a, string_t b, SolidModel &m1, SolidModel &m2) {
		if (!duckdb_3d::BoxesOverlap(ValidSolidBox(a), ValidSolidBox(b))) {
			return false;
		}
		return duckdb_3d::Solid3DIntersects(ReadSolid(a, m1), ReadSolid(b, m2));
	});
}

static void ST_3DDWithinSolidSolidFun(DataChunk &args, ExpressionState &state, Vector &result) {
	SolidModel m1, m2;
	TernaryExecutor::Execute<string_t, string_t, double, bool>(
	    args.data[0], args.data[1], args.data[2], result, args.size(), [&](string_t a, string_t b, double dist) {
		    auto box_a = ValidSolidBox(a);
		    auto box_b = ValidSolidBox(b);
		    if (dist < 0.0 || duckdb_3d::BBoxDistance(box_a, box_b) > dist) {
			    return false;
		    }
		    return duckdb_3d::Solid3DWithin(ReadSolid(a, m1), ReadSolid(b, m2), dist);
	    });
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
		    point.crs = m1.crs;
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
		    line.crs = m1.crs;
		    line.ComputeBBox();
		    auto payload = SerializeGeomPayload(line);
		    return StringVector::AddStringOrBlob(
		        result, string_t(reinterpret_cast<const char *>(payload.data()), payload.size()));
	    });
}

//! ST_3DDisjoint: the negation of the matching ST_3DIntersects overload.
template <void (*INTERSECTS)(DataChunk &, ExpressionState &, Vector &)>
static void ST_3DDisjointFun(DataChunk &args, ExpressionState &state, Vector &result) {
	INTERSECTS(args, state, result);
	UnaryExecutor::Execute<bool, bool>(result, result, args.size(), [](bool intersects) { return !intersects; });
}

//! Every function registered here relates two values, which must be in one CRS;
//! `name` is how the error names the function.
static ScalarFunctionSet InOneCrs(ScalarFunctionSet set, const char *name) {
	for (auto &function : set.functions) {
		function.SetFunctionCallback(WithSameCrs(function.GetFunctionCallback(), name));
	}
	return set;
}

static ScalarFunction InOneCrs(ScalarFunction function, const char *name) {
	function.SetFunctionCallback(WithSameCrs(function.GetFunctionCallback(), name));
	return function;
}

void RegisterDistanceFunctions(ExtensionLoader &loader, const LogicalType &solid_3d_type,
                               const LogicalType &geom_3d_type) {
	ScalarFunctionSet distance_set("st_3ddistance");
	distance_set.AddFunction(ScalarFunction({geom_3d_type, geom_3d_type}, LogicalType::DOUBLE, ST_3DDistanceFun));
	distance_set.AddFunction(
	    ScalarFunction({solid_3d_type, geom_3d_type}, LogicalType::DOUBLE, ST_3DDistanceSolidGeomFun<true>));
	distance_set.AddFunction(
	    ScalarFunction({geom_3d_type, solid_3d_type}, LogicalType::DOUBLE, ST_3DDistanceSolidGeomFun<false>));
	distance_set.AddFunction(
	    ScalarFunction({solid_3d_type, solid_3d_type}, LogicalType::DOUBLE, ST_3DDistanceSolidSolidFun));
	RegisterDocumented(
	    loader, InOneCrs(std::move(distance_set), "ST_3DDistance"),
	    {{"geom1", "geom2"},
	     "Returns the minimum 3D Euclidean distance between two values in input units. A SOLID_3D is the volume it "
	     "bounds, so a geometry inside it is at distance 0; an invalid solid raises.",
	     "ST_3DDistance(ST_Geom3DFromWKB('POINT Z (1 2 3)'::GEOMETRY), ST_Geom3DFromWKB('POINT Z (4 6 3)'::GEOMETRY))",
	     {"distance"}});
	ScalarFunctionSet dwithin_set("st_3ddwithin");
	dwithin_set.AddFunction(
	    ScalarFunction({geom_3d_type, geom_3d_type, LogicalType::DOUBLE}, LogicalType::BOOLEAN, ST_3DDWithinFun));
	dwithin_set.AddFunction(ScalarFunction({solid_3d_type, geom_3d_type, LogicalType::DOUBLE}, LogicalType::BOOLEAN,
	                                       ST_3DDWithinSolidGeomFun<true>));
	dwithin_set.AddFunction(ScalarFunction({geom_3d_type, solid_3d_type, LogicalType::DOUBLE}, LogicalType::BOOLEAN,
	                                       ST_3DDWithinSolidGeomFun<false>));
	dwithin_set.AddFunction(ScalarFunction({solid_3d_type, solid_3d_type, LogicalType::DOUBLE}, LogicalType::BOOLEAN,
	                                       ST_3DDWithinSolidSolidFun));
	RegisterDocumented(
	    loader, InOneCrs(std::move(dwithin_set), "ST_3DDWithin"),
	    {{"geom1", "geom2", "distance"},
	     "Returns whether two values are within the given 3D distance of each other; prunes on bounding boxes, so it "
	     "is cheaper than comparing ST_3DDistance. A SOLID_3D is the volume it bounds. A negative distance returns "
	     "false.",
	     "ST_3DDWithin(ST_Geom3DFromWKB('POINT Z (1 2 3)'::GEOMETRY), ST_Geom3DFromWKB('POINT Z (4 6 3)'::GEOMETRY), "
	     "5.0)",
	     {"distance"}});
	RegisterDocumented(
	    loader,
	    InOneCrs(
	        ScalarFunction("st_3dmaxdistance", {geom_3d_type, geom_3d_type}, LogicalType::DOUBLE, ST_3DMaxDistanceFun),
	        "ST_3DMaxDistance"),
	    {{"geom1", "geom2"},
	     "Returns the maximum 3D Euclidean distance between any two vertices of two GEOM_3D values, in input units.",
	     "ST_3DMaxDistance(ST_Geom3DFromWKB('POINT Z (1 2 3)'::GEOMETRY), ST_Geom3DFromWKB('POINT Z (4 6 "
	     "3)'::GEOMETRY))",
	     {"distance"}});
	RegisterDocumented(loader,
	                   InOneCrs(ScalarFunction("st_3ddfullywithin", {geom_3d_type, geom_3d_type, LogicalType::DOUBLE},
	                                           LogicalType::BOOLEAN, ST_3DDFullyWithinFun),
	                            "ST_3DDFullyWithin"),
	                   {{"geom1", "geom2", "distance"},
	                    "Returns whether the maximum 3D distance between two GEOM_3D values is within the given "
	                    "distance. A negative distance returns false.",
	                    "ST_3DDFullyWithin(ST_Geom3DFromWKB('POINT Z (1 2 3)'::GEOMETRY), ST_Geom3DFromWKB('POINT Z (4 "
	                    "6 3)'::GEOMETRY), 10.0)",
	                    {"distance"}});
	ScalarFunctionSet intersects_set("st_3dintersects");
	intersects_set.AddFunction(ScalarFunction({geom_3d_type, geom_3d_type}, LogicalType::BOOLEAN, ST_3DIntersectsFun));
	intersects_set.AddFunction(
	    ScalarFunction({solid_3d_type, geom_3d_type}, LogicalType::BOOLEAN, ST_3DIntersectsSolidGeomFun<true>));
	intersects_set.AddFunction(
	    ScalarFunction({geom_3d_type, solid_3d_type}, LogicalType::BOOLEAN, ST_3DIntersectsSolidGeomFun<false>));
	intersects_set.AddFunction(
	    ScalarFunction({solid_3d_type, solid_3d_type}, LogicalType::BOOLEAN, ST_3DIntersectsSolidSolidFun));
	RegisterDocumented(
	    loader, InOneCrs(std::move(intersects_set), "ST_3DIntersects"),
	    {{"geom1", "geom2"},
	     "Returns whether two values share a point in 3D, decided exactly; touching counts. A SOLID_3D is the volume "
	     "it bounds; an invalid solid raises.",
	     "ST_3DIntersects(ST_Geom3DFromWKB('POLYGON Z ((0 0 0, 2 0 0, 2 2 0, 0 2 0, 0 0 0))'::GEOMETRY), "
	     "ST_Geom3DFromWKB('POINT Z (1 2 3)'::GEOMETRY))",
	     {"distance"}});
	ScalarFunctionSet disjoint_set("st_3ddisjoint");
	disjoint_set.AddFunction(
	    ScalarFunction({geom_3d_type, geom_3d_type}, LogicalType::BOOLEAN, ST_3DDisjointFun<ST_3DIntersectsFun>));
	disjoint_set.AddFunction(ScalarFunction({solid_3d_type, geom_3d_type}, LogicalType::BOOLEAN,
	                                        ST_3DDisjointFun<ST_3DIntersectsSolidGeomFun<true>>));
	disjoint_set.AddFunction(ScalarFunction({geom_3d_type, solid_3d_type}, LogicalType::BOOLEAN,
	                                        ST_3DDisjointFun<ST_3DIntersectsSolidGeomFun<false>>));
	disjoint_set.AddFunction(ScalarFunction({solid_3d_type, solid_3d_type}, LogicalType::BOOLEAN,
	                                        ST_3DDisjointFun<ST_3DIntersectsSolidSolidFun>));
	RegisterDocumented(
	    loader, InOneCrs(std::move(disjoint_set), "ST_3DDisjoint"),
	    {{"geom1", "geom2"},
	     "Returns whether two values share no point in 3D: the negation of ST_3DIntersects, decided exactly. A "
	     "SOLID_3D is the volume it bounds; an invalid solid raises.",
	     "ST_3DDisjoint(ST_Geom3DFromWKB('POINT Z (0 0 0)'::GEOMETRY), ST_Geom3DFromWKB('POINT Z (1 2 3)'::GEOMETRY))",
	     {"distance"}});
	RegisterDocumented(
	    loader,
	    InOneCrs(ScalarFunction("st_3dclosestpoint", {geom_3d_type, geom_3d_type}, geom_3d_type, ST_3DClosestPointFun),
	             "ST_3DClosestPoint"),
	    {{"geom1", "geom2"},
	     "Returns the Point Z on the first GEOM_3D value nearest to the second.",
	     "ST_3DClosestPoint(ST_Geom3DFromWKB('POLYGON Z ((0 0 0, 2 0 0, 2 2 0, 0 2 0, 0 0 0))'::GEOMETRY), "
	     "ST_Geom3DFromWKB('POINT Z (4 6 3)'::GEOMETRY))",
	     {"distance"}});
	RegisterDocumented(
	    loader,
	    InOneCrs(ScalarFunction("st_3dshortestline", {geom_3d_type, geom_3d_type}, geom_3d_type, ST_3DShortestLineFun),
	             "ST_3DShortestLine"),
	    {{"geom1", "geom2"},
	     "Returns the 2-vertex LineString Z joining the closest points of two GEOM_3D values.",
	     "ST_3DShortestLine(ST_Geom3DFromWKB('POLYGON Z ((0 0 0, 2 0 0, 2 2 0, 0 2 0, 0 0 0))'::GEOMETRY), "
	     "ST_Geom3DFromWKB('POINT Z (4 6 3)'::GEOMETRY))",
	     {"distance"}});
}

} // namespace duckdb
