#include "functions/three_d_functions.hpp"

#include "duckdb/common/string_util.hpp"
#include "duckdb/common/types/geometry_crs.hpp"
#include "duckdb/function/scalar_function.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"

#include <string>
#include <vector>

namespace duckdb {

// ──────────────────────────────────────────────────────────────
// A value's CRS: a string in the payload, empty when there is none. It comes in
// from an EWKB SRID or a GEOMETRY argument's type, every function that returns a
// geometry keeps it, and functions of two values refuse two different ones.
// ──────────────────────────────────────────────────────────────

std::string PayloadCrs(string_t payload) {
	auto data = reinterpret_cast<const uint8_t *>(payload.GetData());
	auto size = payload.GetSize();
	switch (GetPayloadKind(data, size)) {
	case PayloadKind::Solid:
		return std::string(duckdb_3d::ReadSolidPayloadHeader(data, size).crs);
	case PayloadKind::Geom:
		return std::string(duckdb_3d::ReadGeomPayloadHeader(data, size).crs);
	default:
		throw InvalidInputException("argument is not a SOLID_3D or GEOM_3D value");
	}
}

bool SameCrs(const std::string &a, const std::string &b) {
	if (a == b) {
		return true;
	}
	if (a.empty() || b.empty()) {
		return false;
	}
	// Two spellings of one CRS, such as an AUTH:CODE in either case and a PROJJSON
	// carrying that id, are the same: they resolve to one identifier.
	CoordinateReferenceSystem crs_a(a);
	CoordinateReferenceSystem crs_b(b);
	if (crs_a.Equals(crs_b)) {
		return true;
	}
	const auto &id_a = crs_a.GetIdentifier();
	const auto &id_b = crs_b.GetIdentifier();
	return !id_a.empty() && StringUtil::CIEquals(id_a, id_b);
}

static std::string DisplayCrs(const std::string &crs) {
	if (crs.empty()) {
		return "none";
	}
	auto shown = CoordinateReferenceSystem(crs).GetIdentifier();
	return shown.size() > 60 ? shown.substr(0, 57) + "..." : shown;
}

void RequireSameCrs(const std::string &a, const std::string &b, const char *function) {
	if (!SameCrs(a, b)) {
		throw InvalidInputException("%s: the inputs have different CRSs (%s and %s); bring them to one with "
		                            "ST_3DTransform, or set one with ST_3DSetCRS",
		                            function, DisplayCrs(a), DisplayCrs(b));
	}
}

std::string TransformSourceCrs(const std::string &stored, const std::string &source) {
	if (source.empty()) {
		if (stored.empty()) {
			throw InvalidInputException("ST_3DTransform: the value has no CRS; give the source CRS as well, or set "
			                            "one with ST_3DSetCRS");
		}
		return stored;
	}
	if (!stored.empty() && !SameCrs(stored, source)) {
		throw InvalidInputException("ST_3DTransform: the value is in %s, not %s", DisplayCrs(stored),
		                            DisplayCrs(source));
	}
	return source;
}

unique_ptr<FunctionData> CrsBindData::Copy() const {
	return make_uniq<CrsBindData>(crs);
}

bool CrsBindData::Equals(const FunctionData &other) const {
	return crs == other.Cast<CrsBindData>().crs;
}

unique_ptr<FunctionData> GeometryArgumentCrs(const LogicalType &type) {
	if (type.id() != LogicalTypeId::GEOMETRY || !GeoType::HasCRS(type)) {
		return nullptr;
	}
	return make_uniq<CrsBindData>(GeoType::GetCRS(type).GetIdentifier());
}

std::string BoundCrs(ExpressionState &state) {
	auto &bind_info = state.expr.Cast<BoundFunctionExpression>().bind_info;
	return bind_info ? bind_info->Cast<CrsBindData>().crs : std::string();
}

// ST_3DCRS(x) → VARCHAR: the CRS, NULL when there is none.
static void ST_3DCRSFun(DataChunk &args, ExpressionState &state, Vector &result) {
	UnaryExecutor::ExecuteWithNulls<string_t, string_t>(args.data[0], result, args.size(),
	                                                    [&](string_t value, ValidityMask &mask, idx_t idx) {
		                                                    auto crs = PayloadCrs(value);
		                                                    if (crs.empty()) {
			                                                    mask.SetInvalid(idx);
			                                                    return string_t();
		                                                    }
		                                                    return StringVector::AddString(result, crs);
	                                                    });
}

// ST_3DSRID(x) → INTEGER: n for a CRS "EPSG:n", else 0, PostGIS's unknown SRID.
static void ST_3DSRIDFun(DataChunk &args, ExpressionState &state, Vector &result) {
	UnaryExecutor::Execute<string_t, int32_t>(args.data[0], result, args.size(), [&](string_t value) {
		auto crs = PayloadCrs(value);
		constexpr char kPrefix[] = "EPSG:";
		if (crs.compare(0, sizeof(kPrefix) - 1, kPrefix) != 0 || crs.size() == sizeof(kPrefix) - 1 ||
		    crs.size() > sizeof(kPrefix) - 1 + 9) {
			return int32_t(0);
		}
		int32_t code = 0;
		for (size_t k = sizeof(kPrefix) - 1; k < crs.size(); k++) {
			if (crs[k] < '0' || crs[k] > '9') {
				return int32_t(0);
			}
			code = code * 10 + (crs[k] - '0');
		}
		return code;
	});
}

// ST_3DSetCRS(x, crs VARCHAR) → x's type with its CRS replaced; '' clears it.
// Only the label changes: the coordinates are not reprojected.
static void ST_3DSetCRSFun(DataChunk &args, ExpressionState &state, Vector &result) {
	BinaryExecutor::Execute<string_t, string_t, string_t>(
	    args.data[0], args.data[1], result, args.size(), [&](string_t value, string_t crs) {
		    auto data = reinterpret_cast<const uint8_t *>(value.GetData());
		    auto size = value.GetSize();
		    std::vector<uint8_t> bytes;
		    switch (GetPayloadKind(data, size)) {
		    case PayloadKind::Solid:
			    bytes = duckdb_3d::WithSolidPayloadCrs(data, size, crs.GetString());
			    break;
		    case PayloadKind::Geom:
			    bytes = duckdb_3d::WithGeomPayloadCrs(data, size, crs.GetString());
			    break;
		    default:
			    throw InvalidInputException("ST_3DSetCRS: argument is not a SOLID_3D or GEOM_3D value");
		    }
		    return StringVector::AddStringOrBlob(result,
		                                         string_t(reinterpret_cast<const char *>(bytes.data()), bytes.size()));
	    });
}

void RegisterCrsFunctions(ExtensionLoader &loader, const LogicalType &solid_3d_type, const LogicalType &geom_3d_type) {
	ScalarFunctionSet crs_set("st_3dcrs");
	crs_set.AddFunction(ScalarFunction({solid_3d_type}, LogicalType::VARCHAR, ST_3DCRSFun));
	crs_set.AddFunction(ScalarFunction({geom_3d_type}, LogicalType::VARCHAR, ST_3DCRSFun));
	RegisterDocumented(loader, std::move(crs_set),
	                   {{"geom"},
	                    "Returns the CRS a SOLID_3D or GEOM_3D value is in, such as 'EPSG:28992', or NULL when it has "
	                    "none. It comes from an EWKB SRID or a GEOMETRY column's type, and ST_3DSetCRS sets it.",
	                    "ST_3DCRS(ST_3DSetCRS(ST_Geom3DFromWKB('POINT Z (85000 446800 5)'::GEOMETRY), 'EPSG:28992'))",
	                    {"crs"}});
	ScalarFunctionSet srid_set("st_3dsrid");
	srid_set.AddFunction(ScalarFunction({solid_3d_type}, LogicalType::INTEGER, ST_3DSRIDFun));
	srid_set.AddFunction(ScalarFunction({geom_3d_type}, LogicalType::INTEGER, ST_3DSRIDFun));
	RegisterDocumented(loader, std::move(srid_set),
	                   {{"geom"},
	                    "Returns the EPSG code of a value whose CRS is 'EPSG:n', and 0 otherwise (no CRS, or one that "
	                    "is not an EPSG code), as PostGIS reports an unknown SRID.",
	                    "ST_3DSRID(ST_3DSetCRS(ST_Geom3DFromWKB('POINT Z (85000 446800 5)'::GEOMETRY), 'EPSG:28992'))",
	                    {"crs"}});
	ScalarFunctionSet set_crs_set("st_3dsetcrs");
	set_crs_set.AddFunction(ScalarFunction({solid_3d_type, LogicalType::VARCHAR}, solid_3d_type, ST_3DSetCRSFun));
	set_crs_set.AddFunction(ScalarFunction({geom_3d_type, LogicalType::VARCHAR}, geom_3d_type, ST_3DSetCRSFun));
	RegisterDocumented(loader, std::move(set_crs_set),
	                   {{"geom", "crs"},
	                    "Returns the value labelled with the given CRS, such as 'EPSG:28992'; an empty string removes "
	                    "the label. The coordinates are not changed: ST_3DTransform reprojects.",
	                    "ST_3DSetCRS(ST_Geom3DFromWKB('POINT Z (85000 446800 5)'::GEOMETRY), 'EPSG:28992')",
	                    {"crs"}});
}

} // namespace duckdb
