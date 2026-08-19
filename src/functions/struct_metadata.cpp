#include "functions/three_d_functions.hpp"

#include "kernel/metadata_parser.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/types/hugeint.hpp"

#include <string>
#include <vector>

namespace duckdb {

namespace {

//! One face count, dispatched on the bound child type: HUGEINT after the
//! (BLOB, ANY) bind normalisation, INTEGER from the arrow-native overloads.
uint32_t ReadFaceCount(Vector &int_vec, idx_t pos) {
	switch (int_vec.GetType().id()) {
	case LogicalTypeId::HUGEINT: {
		// The (BLOB, ANY) bind normalises `shells` to HUGEINT[][] so every
		// standard integer producer type widens without a pre-executor cast
		// failure; the fits-in-uint32 check runs here — inside the TRY variant's
		// catch — so an out-of-range count fails cleanly. TryCast also rejects
		// negatives.
		uint32_t face_count;
		if (!Hugeint::TryCast<uint32_t>(FlatVector::GetData<hugeint_t>(int_vec)[pos], face_count)) {
			throw InvalidInputException("geometry_properties: shells face count out of range");
		}
		return face_count;
	}
	case LogicalTypeId::INTEGER: {
		auto raw = FlatVector::GetData<int32_t>(int_vec)[pos];
		if (raw < 0) {
			throw InvalidInputException("geometry_properties: expected non-negative shell face count");
		}
		return static_cast<uint32_t>(raw);
	}
	default:
		throw InvalidInputException("geometry_properties: unsupported shells face-count type " +
		                            int_vec.GetType().ToString());
	}
}

} // namespace

duckdb_3d::GeometryMetadata ReadGeometryPropertiesStructRow(Vector &struct_vec, idx_t count, idx_t row) {
	duckdb_3d::GeometryMetadata md;
	auto &child_types = StructType::GetChildTypes(struct_vec.GetType());
	auto &children = StructVector::GetEntries(struct_vec);

	Vector *type_vec = nullptr;
	Vector *shells_vec = nullptr;
	for (idx_t i = 0; i < child_types.size(); i++) {
		if (StringUtil::CIEquals(child_types[i].first, "type")) {
			type_vec = children[i].get();
		} else if (StringUtil::CIEquals(child_types[i].first, "shells")) {
			shells_vec = children[i].get();
		}
	}

	if (type_vec) {
		FlattenIfNeeded(*type_vec, count);
		if (FlatVector::Validity(*type_vec).RowIsValid(row)) {
			auto s = FlatVector::GetData<string_t>(*type_vec)[row];
			md.type = std::string(s.GetData(), s.GetSize());
		}
	}
	if (!shells_vec) {
		return md; // no shells field -> same default as the JSON-text parser
	}
	FlattenIfNeeded(*shells_vec, count);
	if (!FlatVector::Validity(*shells_vec).RowIsValid(row)) {
		return md; // NULL shells -> non-solid type, same default as the JSON-text parser
	}
	auto outer = ListVector::GetData(*shells_vec)[row];
	auto &inner_list = ListVector::GetEntry(*shells_vec); // LIST(HUGEINT) or LIST(INTEGER)
	FlattenIfNeeded(inner_list, ListVector::GetListSize(*shells_vec));
	auto inner_data = ListVector::GetData(inner_list);
	auto &inner_list_validity = FlatVector::Validity(inner_list);
	auto &int_vec = ListVector::GetEntry(inner_list);
	FlattenIfNeeded(int_vec, ListVector::GetListSize(inner_list));
	auto &int_validity = FlatVector::Validity(int_vec);

	std::vector<std::vector<uint32_t>> shells;
	shells.reserve(outer.length);
	for (idx_t j = 0; j < outer.length; j++) {
		// A present `shells` value carries no null nested elements (spec §8): a
		// null per-solid array is malformed input, not "no shells". Reject it
		// rather than reading whatever sits behind the null slot.
		if (!inner_list_validity.RowIsValid(outer.offset + j)) {
			throw InvalidInputException("geometry_properties: null shells entry (no nested list element may be null)");
		}
		auto ie = inner_data[outer.offset + j];
		std::vector<uint32_t> shell;
		shell.reserve(ie.length);
		for (idx_t k = 0; k < ie.length; k++) {
			idx_t pos = ie.offset + k;
			if (!int_validity.RowIsValid(pos)) {
				throw InvalidInputException(
				    "geometry_properties: null shell face count (no nested list element may be null)");
			}
			shell.push_back(ReadFaceCount(int_vec, pos));
		}
		shells.push_back(std::move(shell));
	}
	md.shells = std::move(shells);
	return md;
}

} // namespace duckdb
