#include "functions/three_d_functions.hpp"

#include "duckdb/function/function_set.hpp"
#include "duckdb/function/scalar_function.hpp"

#include "kernel/arrow_native_import.hpp"
#include "kernel/metadata_parser.hpp"
#include "kernel/solid_model.hpp"

#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace duckdb {

// ──────────────────────────────────────────────────────────────
// geometry_properties STRUCT("type" VARCHAR, surfaces JSON, face_semantics
// INTEGER[], shells INTEGER[][]) → the kernel's plain GeometryMetadata.
// duckdb-cityjson's arrow-native-type branch (commit d334b26) types
// geometry_properties_lod* this way instead of VARCHAR JSON text; the shared
// ReadGeometryPropertiesStructRow (functions/struct_metadata.cpp) extracts the
// same shell-grouping information the JSON-text path parses, for this path and
// ST_3DFromWKB's (BLOB, ANY) overload alike. It resolves `type`/`shells` by
// name, so the arrow-native overloads' fixed field order and the WKB overload's
// bind-normalised (reorderable) struct both read correctly, and it accepts the
// producer's INTEGER face counts as well as the WKB bind's normalised HUGEINT
// ones. It lives in the SQL/vectorized layer rather than in
// kernel/metadata_parser.* because it reads a live DuckDB Vector (mirroring how
// a BLOB WKB argument is unwrapped into a plain uint8_t*/size before ParseWKB).
// ──────────────────────────────────────────────────────────────

// ──────────────────────────────────────────────────────────────
// Arrow-native ingestion (arrow-native-type branch): ST_3DFromArrowNative /
// ST_3DTryFromArrowNative consume the nested LIST<...<LIST<INTEGER>>>
// boundaries + LIST<STRUCT<x,y,z DOUBLE>> vertices columns cityparquet-rs /
// duckdb-cityjson write directly — no WKB bytes, no geometry_properties.
// ──────────────────────────────────────────────────────────────

//! boundaries: solid -> shell -> face -> ring -> index, 5 levels of LIST<INTEGER>.
LogicalType ArrowNativeGeometryType() {
	auto ring = LogicalType::LIST(LogicalType::INTEGER);
	auto face = LogicalType::LIST(ring);
	auto shell = LogicalType::LIST(face);
	auto solid = LogicalType::LIST(shell);
	return LogicalType::LIST(solid);
}

//! vertices: a flat pool, LIST<STRUCT<x,y,z DOUBLE>>, referenced by index
//! from the boundaries' innermost ring-index lists.
LogicalType ArrowNativeVerticesType() {
	child_list_t<LogicalType> fields;
	fields.push_back(make_pair("x", LogicalType::DOUBLE));
	fields.push_back(make_pair("y", LogicalType::DOUBLE));
	fields.push_back(make_pair("z", LogicalType::DOUBLE));
	return LogicalType::LIST(LogicalType::STRUCT(std::move(fields)));
}

//! A literal or constant-folded expression (as in a simple `SELECT ...` test
//! query, or Task 1's STRUCT overload before this fix) can produce a
//! non-FLAT_VECTOR at any nesting depth, not only the top level —
//! FlatVector::GetData/IsNull assert genuine flat vectors. `count` is this
//! level's own cardinality (its list's total element count across every row,
//! not the outer chunk's row count — list children are a single vector
//! shared/concatenated across all rows' entries).
//! Walks one row of the boundaries Vector, flattening each nested level as it
//! descends, into the kernel's plain-C++ CSR form (real per-level traversal,
//! not a raw buffer cast: DuckDB ListVectors are list_entry_t pairs into a
//! shared child, with possible non-flat intermediate children).
static duckdb_3d::ArrowNativeBoundaries ExtractArrowNativeBoundaries(Vector &boundaries_vec, idx_t row) {
	using namespace duckdb_3d;
	ArrowNativeBoundaries result;

	// CSR construction mirrors model_builder.cpp exactly: push a leading 0 for
	// each offset array once, then push the running cumulative count once
	// per completed element (shell/face/ring) — offsets[i] is element i's
	// start, offsets[i+1] its end, so pushing "the count so far" right after
	// finishing element i is exactly offsets[i+1].
	result.solid_shell_offsets.push_back(0);
	result.shell_face_offsets.push_back(0);
	result.face_ring_offsets.push_back(0);
	result.ring_vertex_offsets.push_back(0);

	uint32_t total_shells = 0, total_faces = 0, total_rings = 0;

	// Nullability invariant (design doc): "within a non-null geometry cell,
	// no nested list element is itself null — every solid/shell/face/ring
	// entry and every vertex index is present." Each level below checks the
	// CHILD vector's validity for the specific slot about to be dereferenced
	// before reading its list_entry_t/int32 — reading an unchecked null slot
	// is not merely "wrong data", it's genuinely undefined: two identical
	// queries were observed to disagree on whether the same null ring even
	// raised an error, because nothing constrains what bytes sit behind a
	// null slot.
	auto solid_entry = FlatVector::GetData<list_entry_t>(boundaries_vec)[row];
	auto &shell_vec = ListVector::GetEntry(boundaries_vec);
	FlattenIfNeeded(shell_vec, ListVector::GetListSize(boundaries_vec));
	auto &shell_validity = FlatVector::Validity(shell_vec);

	for (idx_t solid_idx = solid_entry.offset; solid_idx < solid_entry.offset + solid_entry.length; solid_idx++) {
		if (!shell_validity.RowIsValid(solid_idx)) {
			throw std::runtime_error("arrow-native geometry: null shell entry (no nested list element may be null)");
		}
		auto shell_entry = FlatVector::GetData<list_entry_t>(shell_vec)[solid_idx];
		auto &face_vec = ListVector::GetEntry(shell_vec);
		FlattenIfNeeded(face_vec, ListVector::GetListSize(shell_vec));
		auto &face_validity = FlatVector::Validity(face_vec);

		for (idx_t shell_idx = shell_entry.offset; shell_idx < shell_entry.offset + shell_entry.length; shell_idx++) {
			if (!face_validity.RowIsValid(shell_idx)) {
				throw std::runtime_error("arrow-native geometry: null face entry (no nested list element may be null)");
			}
			auto face_entry = FlatVector::GetData<list_entry_t>(face_vec)[shell_idx];
			auto &ring_vec = ListVector::GetEntry(face_vec);
			FlattenIfNeeded(ring_vec, ListVector::GetListSize(face_vec));
			auto &ring_validity = FlatVector::Validity(ring_vec);

			for (idx_t face_idx = face_entry.offset; face_idx < face_entry.offset + face_entry.length; face_idx++) {
				if (!ring_validity.RowIsValid(face_idx)) {
					throw std::runtime_error(
					    "arrow-native geometry: null ring entry (no nested list element may be null)");
				}
				auto ring_entry = FlatVector::GetData<list_entry_t>(ring_vec)[face_idx];
				auto &index_vec = ListVector::GetEntry(ring_vec);
				FlattenIfNeeded(index_vec, ListVector::GetListSize(ring_vec));
				auto &index_validity = FlatVector::Validity(index_vec);

				for (idx_t r = ring_entry.offset; r < ring_entry.offset + ring_entry.length; r++) {
					if (!index_validity.RowIsValid(r)) {
						throw std::runtime_error(
						    "arrow-native geometry: null vertex-index list entry (no nested list element may be null)");
					}
					auto idx_ring_entry = FlatVector::GetData<list_entry_t>(index_vec)[r];
					auto &leaf_vec = ListVector::GetEntry(index_vec);
					FlattenIfNeeded(leaf_vec, ListVector::GetListSize(index_vec));
					auto &leaf_validity = FlatVector::Validity(leaf_vec);
					auto leaf_data = FlatVector::GetData<int32_t>(leaf_vec);

					for (idx_t k = idx_ring_entry.offset; k < idx_ring_entry.offset + idx_ring_entry.length; k++) {
						if (!leaf_validity.RowIsValid(k)) {
							throw std::runtime_error(
							    "arrow-native geometry: null vertex-pool index (no nested list element may be null)");
						}
						int32_t raw = leaf_data[k];
						if (raw < 0) {
							throw std::runtime_error("arrow-native geometry: negative vertex-pool index");
						}
						result.ring_vertex_indices.push_back(static_cast<uint32_t>(raw));
					}
					total_rings++;
					result.ring_vertex_offsets.push_back(static_cast<uint32_t>(result.ring_vertex_indices.size()));
				}
				total_faces++;
				result.face_ring_offsets.push_back(total_rings);
			}
			total_shells++;
			result.shell_face_offsets.push_back(total_faces);
		}
		result.solid_shell_offsets.push_back(total_shells);
	}

	return result;
}

//! Walks one row of the vertices Vector into a plain vertex pool.
static std::vector<duckdb_3d::Vertex3D> ExtractArrowNativeVertices(Vector &vertices_vec, idx_t row) {
	using namespace duckdb_3d;
	auto vert_entry = FlatVector::GetData<list_entry_t>(vertices_vec)[row];
	auto &struct_vec = ListVector::GetEntry(vertices_vec);
	auto &children = StructVector::GetEntries(struct_vec);
	auto list_size = ListVector::GetListSize(vertices_vec);
	FlattenIfNeeded(*children[0], list_size);
	FlattenIfNeeded(*children[1], list_size);
	FlattenIfNeeded(*children[2], list_size);
	// Nullability invariant (design doc): "no Struct<x,y,z> entry is null and
	// none of x/y/z is null within a present entry."
	auto &struct_validity = FlatVector::Validity(struct_vec);
	auto &x_validity = FlatVector::Validity(*children[0]);
	auto &y_validity = FlatVector::Validity(*children[1]);
	auto &z_validity = FlatVector::Validity(*children[2]);
	auto x_data = FlatVector::GetData<double>(*children[0]);
	auto y_data = FlatVector::GetData<double>(*children[1]);
	auto z_data = FlatVector::GetData<double>(*children[2]);

	std::vector<Vertex3D> vertices;
	vertices.reserve(vert_entry.length);
	for (idx_t i = vert_entry.offset; i < vert_entry.offset + vert_entry.length; i++) {
		if (!struct_validity.RowIsValid(i) || !x_validity.RowIsValid(i) || !y_validity.RowIsValid(i) ||
		    !z_validity.RowIsValid(i)) {
			throw std::runtime_error("arrow-native geometry: null vertex-pool entry or coordinate "
			                         "(no vertex or coordinate may be null)");
		}
		vertices.push_back(Vertex3D {x_data[i], y_data[i], z_data[i]});
	}
	return vertices;
}

//! Design doc "critical invariant": the physical boundaries/vertices shape is
//! uniform across Solid-family and (padded) surface-family rows, so a single
//! column may legitimately mix them. Consumers MUST dispatch on
//! geometry_properties.type per row, never on physical shape — checking
//! shell-count/solid-count alone cannot distinguish a real single-shell
//! Solid from a padded MultiSurface (they are, by design, shape-identical).
static bool IsSolidFamilyType(const std::string &type) {
	return type == "Solid" || type == "MultiSolid" || type == "CompositeSolid";
}

static bool IsSurfaceFamilyType(const std::string &type) {
	return type == "MultiSurface" || type == "CompositeSurface";
}

//! Shared row logic for ST_3DFromArrowNative/ST_3DTryFromArrowNative, given an
//! already-extracted GeometryMetadata (from either the VARCHAR or STRUCT
//! geometry_properties overload). Checks the family, then builds+serializes.
//! metadata is also passed to the kernel so BuildSolidModelFromArrowNative
//! can regroup shells from metadata.shells — a real producer (confirmed:
//! cityparquet-rs's arrow_geom_write.rs) always pads a Solid's boundaries to
//! one physical shell, so trusting the physical nesting alone would merge
//! every interior shell into the exterior and silently skip
//! CheckInteriorShellWinding.
static std::string BuildSolidPayloadForRow(Vector &boundaries_vec, Vector &vertices_vec, idx_t row,
                                           const duckdb_3d::GeometryMetadata &metadata) {
	using namespace duckdb_3d;
	if (!IsSolidFamilyType(metadata.type)) {
		throw std::runtime_error("geometry_properties.type '" + metadata.type +
		                         "' is not a solid-family type (Solid/MultiSolid/CompositeSolid) — "
		                         "call ST_Geom3DFromArrowNative for surface types");
	}
	auto boundaries = ExtractArrowNativeBoundaries(boundaries_vec, row);
	auto vertices = ExtractArrowNativeVertices(vertices_vec, row);
	auto model = BuildSolidModelFromArrowNative(boundaries, vertices, metadata);
	auto payload = SerializePayload(model);
	return std::string(reinterpret_cast<const char *>(payload.data()), payload.size());
}

//! Shared row logic for ST_Geom3DFromArrowNative/ST_Geom3DTryFromArrowNative.
//! Surface types carry no shells (BuildGeomModelFromArrowNative's own
//! padding-dimension assertion is the only structural check needed), so no
//! metadata-driven regrouping applies here.
static std::string BuildGeomPayloadForRow(Vector &boundaries_vec, Vector &vertices_vec, idx_t row,
                                          const duckdb_3d::GeometryMetadata &metadata) {
	using namespace duckdb_3d;
	if (!IsSurfaceFamilyType(metadata.type)) {
		throw std::runtime_error("geometry_properties.type '" + metadata.type +
		                         "' is not a surface-family type (MultiSurface/CompositeSurface) — "
		                         "call ST_3DFromArrowNative for solid types");
	}
	auto boundaries = ExtractArrowNativeBoundaries(boundaries_vec, row);
	auto vertices = ExtractArrowNativeVertices(vertices_vec, row);
	auto model = BuildGeomModelFromArrowNative(boundaries, vertices);
	auto payload = SerializeGeomPayload(model);
	return std::string(reinterpret_cast<const char *>(payload.data()), payload.size());
}

// ──────────────────────────────────────────────────────────────
// Arrow-native ingestion:
// ST_3DFromArrowNative / ST_3DTryFromArrowNative(boundaries, vertices,
// geometry_properties) → SOLID_3D, plus the ST_Geom3D* twins → GEOM_3D.
//
// One executor covers all eight overloads: {solid, surface} × {plain, TRY} ×
// {JSON VARCHAR metadata, geometry_properties STRUCT}. The VARCHAR overloads
// parse JSON text through the same ParseGeometryProperties the WKB path uses —
// per the design doc, arrow-native keeps geometry_properties as VARCHAR-JSON on
// both paths precisely so this parsing step is identical, not something the new
// path gets to skip. The STRUCT overloads mirror ST_3DFromWKB's struct
// overload: they bind directly against cityparquet-rs's real
// geometry_properties_lod* column (confirmed STRUCT, the same type for WKB and
// arrow-native rows) without an explicit to_json() cast.
//
// `.type` is load-bearing here — it dispatches solid-family vs surface-family
// in the row builders — rather than merely informational as on the WKB path.
// For the surface functions the boundaries' padding to solid-count 1 /
// shell-count 1 (see BuildGeomModelFromArrowNative) is only a defensive
// secondary check.
// ──────────────────────────────────────────────────────────────

//! Shared executor for all eight arrow-native ingestion functions
//! (st_3d[try]fromarrownative and st_geom3d[try]fromarrownative, VARCHAR and
//! STRUCT geometry_properties). BUILD_ROW is BuildSolidPayloadForRow or
//! BuildGeomPayloadForRow; TRY_VARIANT turns per-row failures into NULLs.
using ArrowNativeRowBuilder = std::string (*)(Vector &, Vector &, idx_t, const duckdb_3d::GeometryMetadata &);

template <bool TRY_VARIANT, MetaSource SOURCE, ArrowNativeRowBuilder BUILD_ROW>
static void FromArrowNativeExecutor(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &boundaries_vec = args.data[0];
	auto &vertices_vec = args.data[1];
	auto &meta_vec = args.data[2];
	auto count = args.size();
	bool all_constant = args.AllConstant();

	boundaries_vec.Flatten(count);
	vertices_vec.Flatten(count);
	auto &boundaries_validity = FlatVector::Validity(boundaries_vec);
	auto &vertices_validity = FlatVector::Validity(vertices_vec);
	auto &result_validity = FlatVector::Validity(result);
	auto result_data = FlatVector::GetData<string_t>(result);

	UnifiedVectorFormat meta_data;
	const string_t *meta_strings = nullptr;
	if constexpr (SOURCE == MetaSource::STRUCT_FIELDS) {
		meta_vec.Flatten(count);
	} else {
		meta_vec.ToUnifiedFormat(count, meta_data);
		meta_strings = UnifiedVectorFormat::GetData<string_t>(meta_data);
	}

	for (idx_t i = 0; i < count; i++) {
		bool meta_valid;
		idx_t meta_idx = i;
		if constexpr (SOURCE == MetaSource::STRUCT_FIELDS) {
			meta_valid = FlatVector::Validity(meta_vec).RowIsValid(i);
		} else {
			meta_idx = meta_data.sel->get_index(i);
			meta_valid = meta_data.validity.RowIsValid(meta_idx);
		}
		if (!boundaries_validity.RowIsValid(i) || !vertices_validity.RowIsValid(i) || !meta_valid) {
			result_validity.SetInvalid(i);
			result_data[i] = string_t();
			continue;
		}
		auto process_row = [&]() {
			using namespace duckdb_3d;
			GeometryMetadata metadata;
			if constexpr (SOURCE == MetaSource::STRUCT_FIELDS) {
				metadata = ReadGeometryPropertiesStructRow(meta_vec, count, i);
			} else {
				auto &meta_str = meta_strings[meta_idx];
				metadata = ParseGeometryProperties(std::string(meta_str.GetData(), meta_str.GetSize()));
			}
			auto payload = BUILD_ROW(boundaries_vec, vertices_vec, i, metadata);
			result_data[i] = StringVector::AddStringOrBlob(result, string_t(payload.data(), payload.size()));
		};
		if constexpr (TRY_VARIANT) {
			try {
				process_row();
			} catch (...) {
				result_validity.SetInvalid(i);
				result_data[i] = string_t();
			}
		} else {
			process_row();
		}
	}

	if (all_constant) {
		result.SetVectorType(VectorType::CONSTANT_VECTOR);
	}
}

//! geometry_properties STRUCT("type" VARCHAR, surfaces JSON, face_semantics
//! INTEGER[], shells INTEGER[][]) — the shape duckdb-cityjson's arrow-native-type
//! branch (commit d334b26) and cityparquet-rs both emit for
//! geometry_properties_lod* instead of/alongside VARCHAR JSON text.
static LogicalType GeometryPropertiesStructType() {
	child_list_t<LogicalType> geom_props_fields;
	geom_props_fields.push_back(make_pair("type", LogicalType::VARCHAR));
	geom_props_fields.push_back(make_pair("surfaces", LogicalType::VARCHAR));
	geom_props_fields.push_back(make_pair("face_semantics", LogicalType::LIST(LogicalType::INTEGER)));
	geom_props_fields.push_back(make_pair("shells", LogicalType::LIST(LogicalType::LIST(LogicalType::INTEGER))));
	return LogicalType::STRUCT(std::move(geom_props_fields));
}

void RegisterArrowNativeFunctions(ExtensionLoader &loader, const LogicalType &solid_3d_type,
                                  const LogicalType &geom_3d_type) {
	// Declared here (before any registration that needs it) so both the
	// GEOM_3D and SOLID_3D arrow-native STRUCT overloads below can share it.
	auto geometry_properties_struct_type = GeometryPropertiesStructType();

	// ST_Geom3DFromArrowNative / ST_Geom3DTryFromArrowNative: VARCHAR and STRUCT
	// geometry_properties overloads (STRUCT binds directly against
	// cityparquet-rs's real geometry_properties_lod* column).
	ScalarFunctionSet geom3d_from_arrow_native_set("st_geom3dfromarrownative");
	auto geom3d_from_arrow_native =
	    ScalarFunction({ArrowNativeGeometryType(), ArrowNativeVerticesType(), LogicalType::VARCHAR}, geom_3d_type,
	                   FromArrowNativeExecutor<false, MetaSource::JSON_TEXT, BuildGeomPayloadForRow>);
	geom3d_from_arrow_native.SetNullHandling(FunctionNullHandling::SPECIAL_HANDLING);
	geom3d_from_arrow_native_set.AddFunction(geom3d_from_arrow_native);
	auto geom3d_from_arrow_native_struct =
	    ScalarFunction({ArrowNativeGeometryType(), ArrowNativeVerticesType(), geometry_properties_struct_type},
	                   geom_3d_type, FromArrowNativeExecutor<false, MetaSource::STRUCT_FIELDS, BuildGeomPayloadForRow>);
	geom3d_from_arrow_native_struct.SetNullHandling(FunctionNullHandling::SPECIAL_HANDLING);
	geom3d_from_arrow_native_set.AddFunction(geom3d_from_arrow_native_struct);
	loader.RegisterFunction(geom3d_from_arrow_native_set);

	ScalarFunctionSet geom3d_try_from_arrow_native_set("st_geom3dtryfromarrownative");
	auto geom3d_try_from_arrow_native =
	    ScalarFunction({ArrowNativeGeometryType(), ArrowNativeVerticesType(), LogicalType::VARCHAR}, geom_3d_type,
	                   FromArrowNativeExecutor<true, MetaSource::JSON_TEXT, BuildGeomPayloadForRow>);
	geom3d_try_from_arrow_native.SetNullHandling(FunctionNullHandling::SPECIAL_HANDLING);
	geom3d_try_from_arrow_native_set.AddFunction(geom3d_try_from_arrow_native);
	auto geom3d_try_from_arrow_native_struct =
	    ScalarFunction({ArrowNativeGeometryType(), ArrowNativeVerticesType(), geometry_properties_struct_type},
	                   geom_3d_type, FromArrowNativeExecutor<true, MetaSource::STRUCT_FIELDS, BuildGeomPayloadForRow>);
	geom3d_try_from_arrow_native_struct.SetNullHandling(FunctionNullHandling::SPECIAL_HANDLING);
	geom3d_try_from_arrow_native_set.AddFunction(geom3d_try_from_arrow_native_struct);
	loader.RegisterFunction(geom3d_try_from_arrow_native_set);

	// ST_3DFromArrowNative / ST_3DTryFromArrowNative(boundaries, vertices, geometry_properties) -> SOLID_3D
	// VARCHAR and STRUCT geometry_properties overloads, mirroring the WKB set above.
	ScalarFunctionSet from_arrow_native_set("st_3dfromarrownative");
	auto from_arrow_native =
	    ScalarFunction({ArrowNativeGeometryType(), ArrowNativeVerticesType(), LogicalType::VARCHAR}, solid_3d_type,
	                   FromArrowNativeExecutor<false, MetaSource::JSON_TEXT, BuildSolidPayloadForRow>);
	from_arrow_native.SetNullHandling(FunctionNullHandling::SPECIAL_HANDLING);
	from_arrow_native_set.AddFunction(from_arrow_native);
	auto from_arrow_native_struct = ScalarFunction(
	    {ArrowNativeGeometryType(), ArrowNativeVerticesType(), geometry_properties_struct_type}, solid_3d_type,
	    FromArrowNativeExecutor<false, MetaSource::STRUCT_FIELDS, BuildSolidPayloadForRow>);
	from_arrow_native_struct.SetNullHandling(FunctionNullHandling::SPECIAL_HANDLING);
	from_arrow_native_set.AddFunction(from_arrow_native_struct);
	loader.RegisterFunction(from_arrow_native_set);

	ScalarFunctionSet try_from_arrow_native_set("st_3dtryfromarrownative");
	auto try_from_arrow_native =
	    ScalarFunction({ArrowNativeGeometryType(), ArrowNativeVerticesType(), LogicalType::VARCHAR}, solid_3d_type,
	                   FromArrowNativeExecutor<true, MetaSource::JSON_TEXT, BuildSolidPayloadForRow>);
	try_from_arrow_native.SetNullHandling(FunctionNullHandling::SPECIAL_HANDLING);
	try_from_arrow_native_set.AddFunction(try_from_arrow_native);
	auto try_from_arrow_native_struct = ScalarFunction(
	    {ArrowNativeGeometryType(), ArrowNativeVerticesType(), geometry_properties_struct_type}, solid_3d_type,
	    FromArrowNativeExecutor<true, MetaSource::STRUCT_FIELDS, BuildSolidPayloadForRow>);
	try_from_arrow_native_struct.SetNullHandling(FunctionNullHandling::SPECIAL_HANDLING);
	try_from_arrow_native_set.AddFunction(try_from_arrow_native_struct);
	loader.RegisterFunction(try_from_arrow_native_set);
}

} // namespace duckdb
