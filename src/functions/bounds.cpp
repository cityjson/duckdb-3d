#include "functions/three_d_functions.hpp"

#include "duckdb/common/types/geometry.hpp"
#include "duckdb/common/vector/flat_vector.hpp"
#include "duckdb/common/vector/vector_iterator.hpp"
#include "duckdb/common/vector/vector_writer.hpp"
#include "duckdb/function/aggregate_function.hpp"
#include "duckdb/function/function_set.hpp"
#include "duckdb/function/scalar_function.hpp"
#include "duckdb/parser/parsed_data/create_aggregate_function_info.hpp"

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

namespace duckdb {

// ──────────────────────────────────────────────────────────────
// Bounding boxes: ST_3DBounds, ST_3DXMin .. ST_3DZMax, ST_3DExtent
// ──────────────────────────────────────────────────────────────
// Every payload keeps its box in the front header, so these never read the body.

//! The payload's box into `box`, or false for an empty geometry, which has none.
static bool PayloadBBox(string_t payload, const char *function, duckdb_3d::BBox3D &box) {
	auto data = reinterpret_cast<const uint8_t *>(payload.GetData());
	auto size = payload.GetSize();
	switch (GetPayloadKind(data, size)) {
	case PayloadKind::Solid:
		box = duckdb_3d::ReadSolidPayloadHeader(data, size).bbox;
		return true;
	case PayloadKind::Geom: {
		auto info = duckdb_3d::ReadGeomPayloadHeader(data, size);
		box = info.bbox;
		// POINT EMPTY is stored as one vertex of NaNs, whose box is no box.
		return info.vertex_count > 0 && box.min_x <= box.max_x && box.min_y <= box.max_y && box.min_z <= box.max_z;
	}
	default:
		throw InvalidInputException("%s: argument is not a SOLID_3D or GEOM_3D value", function);
	}
}

//! Struct children name LogicalTypeId, not the LogicalType:: constants: pair's
//! forwarding constructor would bind a reference to the constexpr member, which
//! C++17 emits as an inline variable here and DuckDB (C++11) defines out of
//! line, and the static link then sees two definitions.
LogicalType BBox3DType() {
	child_list_t<LogicalType> children;
	for (auto name : {"min_x", "min_y", "min_z", "max_x", "max_y", "max_z"}) {
		children.push_back({name, LogicalTypeId::DOUBLE});
	}
	return LogicalType::STRUCT(std::move(children));
}

using BBoxWriter = VectorWriter<VectorStructType<double, double, double, double, double, double>>;

//! Write `box` as the writer's next BBox3DType row.
static void WriteBBox(BBoxWriter &writer, const duckdb_3d::BBox3D &box) {
	writer.WriteValue([&](auto &min_x, auto &min_y, auto &min_z, auto &max_x, auto &max_y, auto &max_z) {
		min_x.WriteValue(box.min_x);
		min_y.WriteValue(box.min_y);
		min_z.WriteValue(box.min_z);
		max_x.WriteValue(box.max_x);
		max_y.WriteValue(box.max_y);
		max_z.WriteValue(box.max_z);
	});
}

static void ST_3DBoundsFun(DataChunk &args, ExpressionState &state, Vector &result) {
	auto writer =
	    FlatVector::Writer<VectorStructType<double, double, double, double, double, double>>(result, args.size());
	for (auto entry : args.data[0].Values<string_t>()) {
		duckdb_3d::BBox3D box;
		if (!entry.IsValid() || !PayloadBBox(entry.GetValue(), "ST_3DBounds", box)) {
			writer.WriteNull();
			continue;
		}
		WriteBBox(writer, box);
	}
}

template <double duckdb_3d::BBox3D::*FIELD>
static void BBoxFieldFun(DataChunk &args, ExpressionState &state, Vector &result) {
	UnaryExecutor::Execute<string_t, double>(args.data[0], result, [](string_t payload) -> optional<double> {
		duckdb_3d::BBox3D box;
		if (!PayloadBBox(payload, "bounding box accessor", box)) {
			return {};
		}
		return box.*FIELD;
	});
}

// ST_3DEnvelope(x) → GEOMETRY: the box's footprint, the rectangle of its X and Y
// ranges, as a core GEOMETRY; spatial's spatial join takes it as a candidate
// filter. A box with no width on an axis gives a line, with none on either a
// point, as PostGIS's ST_Envelope does.
static void AppendU32(std::vector<uint8_t> &out, uint32_t v) {
	for (int i = 0; i < 4; i++) {
		out.push_back(static_cast<uint8_t>(v >> (8 * i)));
	}
}

static void AppendPoint(std::vector<uint8_t> &out, double x, double y) {
	for (double v : {x, y}) {
		uint64_t bits;
		memcpy(&bits, &v, sizeof(bits));
		for (int i = 0; i < 8; i++) {
			out.push_back(static_cast<uint8_t>(bits >> (8 * i)));
		}
	}
}

static void ST_3DEnvelopeFun(DataChunk &args, ExpressionState &state, Vector &result) {
	std::vector<uint8_t> wkb;
	auto &heap = StringVector::GetStringHeap(result);
	UnaryExecutor::Execute<string_t, string_t>(args.data[0], result, [&](string_t payload) -> optional<string_t> {
		duckdb_3d::BBox3D box;
		if (!PayloadBBox(payload, "ST_3DEnvelope", box)) {
			return {};
		}
		wkb.clear();
		wkb.push_back(1);
		const bool flat_x = box.min_x == box.max_x, flat_y = box.min_y == box.max_y;
		if (flat_x && flat_y) {
			AppendU32(wkb, 1);
			AppendPoint(wkb, box.min_x, box.min_y);
		} else if (flat_x || flat_y) {
			AppendU32(wkb, 2);
			AppendU32(wkb, 2);
			AppendPoint(wkb, box.min_x, box.min_y);
			AppendPoint(wkb, box.max_x, box.max_y);
		} else {
			AppendU32(wkb, 3);
			AppendU32(wkb, 1);
			AppendU32(wkb, 5);
			AppendPoint(wkb, box.min_x, box.min_y);
			AppendPoint(wkb, box.max_x, box.min_y);
			AppendPoint(wkb, box.max_x, box.max_y);
			AppendPoint(wkb, box.min_x, box.max_y);
			AppendPoint(wkb, box.min_x, box.min_y);
		}
		string_t geometry;
		Geometry::FromBinary(string_t(reinterpret_cast<const char *>(wkb.data()), wkb.size()), geometry, heap, true);
		return geometry;
	});
}

// ST_3DExtent: the box of every box. The state keeps the first CRS it sees, so
// boxes in two CRSs are refused rather than merged.
struct ExtentState {
	bool is_set;
	duckdb_3d::BBox3D box;
	char *crs;
	uint32_t crs_size;
};

struct ExtentOperation {
	template <class STATE>
	static void Initialize(STATE &state) {
		state.is_set = false;
		state.box = {};
		state.crs = nullptr;
		state.crs_size = 0;
	}

	template <class STATE>
	static void Absorb(STATE &target, const duckdb_3d::BBox3D &box, const char *crs, uint32_t crs_size) {
		if (!target.is_set) {
			target.is_set = true;
			target.box = box;
			target.crs_size = crs_size;
			target.crs = crs_size == 0 ? nullptr : new char[crs_size];
			if (crs_size != 0) {
				memcpy(target.crs, crs, crs_size);
			}
			return;
		}
		if (crs_size != target.crs_size || (crs_size != 0 && memcmp(crs, target.crs, crs_size) != 0)) {
			RequireSameCrs(std::string(target.crs == nullptr ? "" : target.crs, target.crs_size),
			               std::string(crs == nullptr ? "" : crs, crs_size), "ST_3DExtent");
		}
		target.box.min_x = std::min(target.box.min_x, box.min_x);
		target.box.min_y = std::min(target.box.min_y, box.min_y);
		target.box.min_z = std::min(target.box.min_z, box.min_z);
		target.box.max_x = std::max(target.box.max_x, box.max_x);
		target.box.max_y = std::max(target.box.max_y, box.max_y);
		target.box.max_z = std::max(target.box.max_z, box.max_z);
	}

	template <class INPUT_TYPE, class STATE, class OP>
	static void Operation(STATE &state, const INPUT_TYPE &input, AggregateUnaryInput &) {
		duckdb_3d::BBox3D box;
		if (!PayloadBBox(input, "ST_3DExtent", box)) {
			return;
		}
		auto crs = PayloadCrs(input);
		Absorb(state, box, crs.data(), static_cast<uint32_t>(crs.size()));
	}

	template <class INPUT_TYPE, class STATE, class OP>
	static void ConstantOperation(STATE &state, const INPUT_TYPE &input, AggregateUnaryInput &unary_input, idx_t) {
		Operation<INPUT_TYPE, STATE, OP>(state, input, unary_input);
	}

	template <class STATE, class OP>
	static void Combine(const STATE &source, STATE &target, AggregateInputData &) {
		if (source.is_set) {
			Absorb(target, source.box, source.crs, source.crs_size);
		}
	}

	template <class STATE>
	static void Destroy(STATE &state, AggregateInputData &) {
		delete[] state.crs;
		state.crs = nullptr;
	}

	static bool IgnoreNull() {
		return true;
	}
};

static void ExtentFinalize(Vector &states, AggregateFinalizeInputData &, Vector &result, idx_t count, idx_t offset) {
	auto writer =
	    FlatVector::Writer<VectorStructType<double, double, double, double, double, double>>(result, count, offset);
	for (auto entry : states.Values<ExtentState *>()) {
		auto &state = *entry.GetValue();
		if (!state.is_set) {
			writer.WriteNull();
			continue;
		}
		WriteBBox(writer, state.box);
	}
}

void RegisterBoundsFunctions(ExtensionLoader &loader, const LogicalType &solid_3d_type,
                             const LogicalType &geom_3d_type) {
	auto bbox_type = BBox3DType();
	ScalarFunctionSet bounds_set("st_3dbounds");
	bounds_set.AddFunction(ScalarFunction({solid_3d_type}, bbox_type, ST_3DBoundsFun));
	bounds_set.AddFunction(ScalarFunction({geom_3d_type}, bbox_type, ST_3DBoundsFun));
	RegisterDocumented(loader, std::move(bounds_set),
	                   {{"geom"},
	                    "Returns the 3D bounding box of a SOLID_3D or GEOM_3D value as STRUCT(min_x, min_y, min_z, "
	                    "max_x, max_y, max_z), read from the cached box.",
	                    "ST_3DBounds(ST_3DExtrude(ST_Geom3DFromWKB('POLYGON Z ((0 0 0, 2 0 0, 2 2 0, 0 2 0, 0 0 "
	                    "0))'::GEOMETRY), 3.0))",
	                    {"introspection"}});

	struct Field {
		const char *name;
		const char *display;
		const char *what;
		scalar_function_t function;
	};
	const Field fields[] = {
	    {"st_3dxmin", "ST_3DXMin", "minimum X", BBoxFieldFun<&duckdb_3d::BBox3D::min_x>},
	    {"st_3dxmax", "ST_3DXMax", "maximum X", BBoxFieldFun<&duckdb_3d::BBox3D::max_x>},
	    {"st_3dymin", "ST_3DYMin", "minimum Y", BBoxFieldFun<&duckdb_3d::BBox3D::min_y>},
	    {"st_3dymax", "ST_3DYMax", "maximum Y", BBoxFieldFun<&duckdb_3d::BBox3D::max_y>},
	    {"st_3dzmin", "ST_3DZMin", "minimum Z", BBoxFieldFun<&duckdb_3d::BBox3D::min_z>},
	    {"st_3dzmax", "ST_3DZMax", "maximum Z", BBoxFieldFun<&duckdb_3d::BBox3D::max_z>},
	};
	for (auto &field : fields) {
		ScalarFunctionSet set(field.name);
		set.AddFunction(ScalarFunction({solid_3d_type}, LogicalType::DOUBLE, field.function));
		set.AddFunction(ScalarFunction({geom_3d_type}, LogicalType::DOUBLE, field.function));
		const string display = field.display;
		RegisterDocumented(loader, std::move(set),
		                   {{"geom"},
		                    StringUtil::Format("Returns the %s coordinate of a SOLID_3D or GEOM_3D value, read from "
		                                       "its cached bounding box.",
		                                       field.what),
		                    display + "(ST_3DExtrude(ST_Geom3DFromWKB('POLYGON Z ((0 0 0, 2 0 0, 2 2 0, 0 2 0, 0 0 "
		                              "0))'::GEOMETRY), 3.0))",
		                    {"introspection"}});
	}

	ScalarFunctionSet envelope_set("st_3denvelope");
	envelope_set.AddFunction(ScalarFunction({solid_3d_type}, LogicalType::GEOMETRY(), ST_3DEnvelopeFun));
	envelope_set.AddFunction(ScalarFunction({geom_3d_type}, LogicalType::GEOMETRY(), ST_3DEnvelopeFun));
	RegisterDocumented(loader, std::move(envelope_set),
	                   {{"geom"},
	                    "Returns the footprint of a SOLID_3D or GEOM_3D value's bounding box as a 2D GEOMETRY: the "
	                    "rectangle of its X and Y ranges (a line or a point where a range has no width), NULL for an "
	                    "empty value. Its 2D distance to another envelope never exceeds the two values' 3D distance, "
	                    "so spatial's spatial join on ST_DWithin of the envelopes generates every candidate "
	                    "ST_3DDWithin needs.",
	                    "ST_3DEnvelope(ST_3DExtrude(ST_Geom3DFromWKB('POLYGON Z ((0 0 0, 2 0 0, 2 2 0, 0 2 0, 0 0 "
	                    "0))'::GEOMETRY), 3.0))",
	                    {"introspection"}});

	AggregateFunctionSet extent_set("st_3dextent");
	for (auto &type : {solid_3d_type, geom_3d_type}) {
		AggregateFunction extent(
		    {type}, bbox_type, AggregateFunction::StateSize<ExtentState>,
		    AggregateFunction::StateInitialize<ExtentState, ExtentOperation, AggregateDestructorType::LEGACY>,
		    AggregateFunction::UnaryScatterUpdate<ExtentState, string_t, ExtentOperation>,
		    AggregateFunction::StateCombine<ExtentState, ExtentOperation>, ExtentFinalize,
		    FunctionNullHandling::DEFAULT_NULL_HANDLING,
		    AggregateFunction::UnaryClusterUpdateCallback<ExtentState, string_t, ExtentOperation>());
		extent.SetStateDestructorCallback(AggregateFunction::StateDestroy<ExtentState, ExtentOperation>);
		NameParameters(extent, {"geom"});
		extent_set.AddFunction(std::move(extent));
	}
	CreateAggregateFunctionInfo info(std::move(extent_set));
	info.on_conflict = OnCreateConflict::ALTER_ON_CONFLICT;
	FunctionDescription description;
	description.parameter_names = {"geom"};
	description.description = "Aggregate: the 3D bounding box of every SOLID_3D or GEOM_3D value in the group, as "
	                          "ST_3DBounds returns it; NULLs are skipped, and values in different CRSs are refused.";
	description.examples = {"ST_3DExtent(geom)"};
	description.categories = {"introspection", "aggregate"};
	info.descriptions.push_back(std::move(description));
	loader.RegisterFunction(std::move(info));
}

} // namespace duckdb
