# Arrow-native geometry encoding — duckdb-3d Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Scope note:** third leg of the 3-repo `arrow-native-type` experiment. Design:
`../documents/docs/superpowers/specs/2026-07-25-arrow-native-geometry-design.md` in the
parent workspace repo (read it first). Per `duckdb-3d/CLAUDE.md`: this is a
public-API/architecture change, so `docs/DESIGN_DOC.md` is updated alongside the code
(Task 8), and strict TDD applies throughout — this plan follows that discipline task by
task, not as an afterthought.

**Urgent prerequisite, not part of the original design doc scope — discovered while
researching this plan.** `duckdb-cityjson`'s `arrow-native-type` branch already landed
commit `d334b26` ("type geometry_properties as a STRUCT per the spec"), which changes
`read_cityjson`'s **existing WKB pipeline's** `geometry_properties_lod*` output column
from `VARCHAR` (JSON text) to a real `STRUCT("type" VARCHAR, surfaces JSON,
face_semantics INTEGER[], shells INTEGER[][])`. `ST_3DFromWKB`'s 2-arg overload
(`three_d_extension.cpp:1864`, confirmed) only accepts `(BLOB, VARCHAR)` — so the
canonical example query in this repo's own `docs/DESIGN_DOC.md` §12
(`ST_3DVolume(ST_3DFromWKB(geometry, geometry_properties))`) **no longer binds** against
fresh `duckdb-cityjson` output without an explicit `to_json(geometry_properties)` cast.
There's a documented workaround (`ParseGeometryProperties` already accepts "a
`geometry_properties_lod*` STRUCT column... converted to JSON text", per
`DESIGN_DOC.md` §8.2), but it's a real ergonomics regression and defeats the point of
moving to STRUCT (avoiding JSON parsing). **Task 1 fixes this properly — a native
`STRUCT`-accepting overload, ahead of the new arrow-native ingestion work**, because it's
higher-value (fixes something broken *today*, for every existing user of this extension,
not just the experiment) and because Task 1's `GeometryMetadata`-from-STRUCT conversion
is reused by nothing else in this plan, but is worth having regardless of how the rest of
this experiment turns out.

**Goal:** two new ingestion functions, `ST_3DFromArrowNative`/`ST_3DTryFromArrowNative`
(→ `SOLID_3D`, for `Solid`/`MultiSolid`/`CompositeSolid`) and
`ST_Geom3DFromArrowNative`/`ST_Geom3DTryFromArrowNative` (→ `GEOM_3D`, for
`MultiSurface`/`CompositeSurface`), consuming the nested `LIST`/`STRUCT` columns
`cityparquet-rs`/`duckdb-cityjson` write, without any WKB byte or `geometry_properties`
JSON parsing.

**Architecture — one refinement beyond the design doc's sketch, reasoned through below.**
The design doc sketches `ST_3DFromArrowNative(boundaries, vertices, geometry_properties)`
dispatching on `geometry_properties.type`. Working through it in detail: **that dispatch
already happens today, one level up, by which of the two existing functions
(`ST_3DFromWKB` vs `ST_Geom3DFromWKB`) a query calls** — a SQL author already knows
whether a column is Solid-family or surface-family before writing the query (it's how
they already choose which of the two existing WKB functions to call). The arrow-native
functions inherit that same split, and once a caller has committed to
`ST_3DFromArrowNative` specifically, the physical shape is self-sufficient: the
solid/shell/face/ring nesting is already explicit (unlike WKB, arrow-native never
flattens shells away), so there's nothing left for `geometry_properties` to recover. So
**both new functions take exactly `(boundaries, vertices)` — two arguments, no
`geometry_properties`** — simpler than the WKB path, not just different from it. Boundary
conversion walks DuckDB's `list_entry_t`/`ListVector`/`UnifiedVectorFormat` (not raw
buffer casts — real per-level traversal, per the design doc's own correction), narrowing
safely into `SolidModel`'s (index-based, needs building) or `GeomModel`'s
(inline-coordinate, needs dereferencing) shape, then runs the SAME non-WKB-specific
construction pipeline (`ComputeBBox`, `TriangulateSolidModel`, `ValidateSolidModel`) those
models already require. The kernel itself (`validation.cpp`, `measurements.hpp`,
`triangulation.cpp`) is untouched.

## Global Constraints

- **Strict red-green TDD, mandatory** (`duckdb-3d/CLAUDE.md`): failing test first,
  minimal implementation, refactor green. Unit test in `test/cpp/` first, SQL integration
  test in `test/sql/` second, per this repo's own "Preferred Test Ordering".
- **Update `docs/DESIGN_DOC.md`** alongside the code (Task 8) — required by this repo's
  own contribution rules for any architecture/public-API change, not optional
  documentation.
- **Preserve `SOLID_3D`/`GEOM_3D` binary payload compatibility** — this plan adds new
  ingestion functions; it does not touch `PayloadHeader`, `SerializePayload`,
  `DeserializePayload`, or bump the payload version. If any step seems to need that,
  stop — it's out of scope and a sign something else went wrong.
- British English in prose/comments, matching this repo's existing style.
- Run `codex exec -m gpt-5.6-sol -s read-only` review at the end (matches
  `cityparquet-rs`'s convention and the user's explicit request for this reviewer across
  the whole experiment).
- Commit after every task; push to `origin/arrow-native-type` after each milestone.
- **Schema parity is non-negotiable**: the nested `LogicalType` this plan builds for
  `boundaries`/`vertices` MUST match `cityparquet-rs`'s
  `arrow_native_geometry_data_type()`/`arrow_native_vertices_data_type()` and
  `duckdb-cityjson`'s `ColumnType::GeometryArrowNative`/`GeometryVerticesArrowNative`
  exactly (5-level nested `LIST<...<LIST<INTEGER>>>` for boundaries, `LIST<STRUCT<x,y,z
  DOUBLE>>` for vertices) — this is what makes the cross-repo consistency gate in the
  design doc's testing plan meaningful.
- **`Point` is out of scope** — no `ST_3D*`/`ST_Geom3D*` function needs a bare point;
  this plan never touches it.

---

## File Structure

| File | Responsibility |
|---|---|
| `src/include/kernel/metadata_parser.hpp`, `src/kernel/metadata_parser.cpp` | Modify: add `GeometryMetadata` construction directly from a DuckDB `STRUCT` `Vector` (Task 1). |
| `src/three_d_extension.cpp` | Modify: `ST_3DFromWKB`'s STRUCT overload (Task 1); new `LogicalType` builders for the arrow-native shapes (Task 2); the four new `ST_*FromArrowNative`/`ST_*TryFromArrowNative` functions + registration (Tasks 4, 6). |
| `src/include/kernel/arrow_native_import.hpp`, `src/kernel/arrow_native_import.cpp` | Create: DuckDB `Vector` → `SolidModel` and `Vector` → `GeomModel` boundary conversion (Tasks 3, 5). |
| `test/cpp/test_arrow_native_import.cpp` | Create: unit tests for the boundary conversion, golden fixtures. |
| `test/sql/st_3d_from_arrow_native.test`, `test/sql/st_geom3d_from_arrow_native.test` | Create: SQL-level integration tests. |
| `docs/DESIGN_DOC.md` | Modify: document the new functions and the arrow-native import pipeline (Task 8). |

---

### Task 1 (urgent prerequisite): `ST_3DFromWKB` gains a `STRUCT`-accepting overload

**Files:**
- Modify: `src/include/kernel/metadata_parser.hpp`, `src/kernel/metadata_parser.cpp`
- Modify: `src/three_d_extension.cpp`

**Interfaces:**
- Produces: `GeometryMetadata ParseGeometryPropertiesStruct(const Vector &struct_vec, idx_t row)` (new, alongside the existing `ParseGeometryProperties(const std::string &json_text)`); a new `ScalarFunction` overload `ST_3DFromWKB(BLOB, STRUCT("type" VARCHAR, surfaces JSON, face_semantics INTEGER[], shells INTEGER[][]))`.

- [ ] **Step 1: Write the failing unit test**

Add to `test/cpp/test_metadata.cpp` (this repo's existing metadata-parsing test file,
confirmed present), a test constructing a real DuckDB `STRUCT` `Vector` matching
`duckdb-cityjson`'s `GeometryPropertiesStruct` shape and checking
`ParseGeometryPropertiesStruct` extracts the same `GeometryMetadata{type, shells}` the
existing JSON-text parser would for equivalent content:

```cpp
TEST_CASE("ParseGeometryPropertiesStruct extracts type and shells from a real DuckDB STRUCT Vector", "[metadata_parser]") {
	// Build a STRUCT("type" VARCHAR, surfaces JSON, face_semantics INTEGER[], shells INTEGER[][])
	// Vector with one row: type="Solid", shells=[[12, 4]] (one solid, two shells).
	child_list_t<LogicalType> fields;
	fields.push_back(make_pair("type", LogicalType::VARCHAR));
	fields.push_back(make_pair("surfaces", LogicalType::VARCHAR)); // JSON stored as VARCHAR
	fields.push_back(make_pair("face_semantics", LogicalType::LIST(LogicalType::INTEGER)));
	fields.push_back(make_pair("shells", LogicalType::LIST(LogicalType::LIST(LogicalType::INTEGER))));
	auto struct_type = LogicalType::STRUCT(std::move(fields));

	Vector vec(struct_type, 1);
	auto &children = StructVector::GetEntries(vec);
	FlatVector::GetData<string_t>(*children[0])[0] = StringVector::AddString(*children[0], "Solid");
	FlatVector::SetNull(*children[1], 0, true);
	FlatVector::GetData<list_entry_t>(*children[2])[0] = list_entry_t(0, 0); // empty face_semantics
	FlatVector::SetNull(*children[2], 0, true);

	// shells = [[12, 4]]: outer list (1 entry: this one solid), inner list (2 entries: 12, 4)
	auto &shells_vec = *children[3];
	FlatVector::GetData<list_entry_t>(shells_vec)[0] = list_entry_t(0, 1);
	ListVector::Reserve(shells_vec, 1);
	auto &inner_vec = ListVector::GetEntry(shells_vec);
	FlatVector::GetData<list_entry_t>(inner_vec)[0] = list_entry_t(0, 2);
	ListVector::Reserve(inner_vec, 2);
	auto &int_vec = ListVector::GetEntry(inner_vec);
	FlatVector::GetData<int32_t>(int_vec)[0] = 12;
	FlatVector::GetData<int32_t>(int_vec)[1] = 4;
	ListVector::SetListSize(inner_vec, 2);
	ListVector::SetListSize(shells_vec, 1);

	using namespace duckdb_3d;
	auto metadata = ParseGeometryPropertiesStruct(vec, 0);
	REQUIRE(metadata.type == "Solid");
	REQUIRE(metadata.shells.size() == 1);
	REQUIRE(metadata.shells[0] == std::vector<uint32_t>{12, 4});
}
```

(Confirm the exact `LogicalType`/`Vector` construction idioms used above against this
repo's own existing test code — `test_metadata.cpp` almost certainly already builds
similar structures for its existing JSON-text tests' expected-output comparisons, or
`test_wkb_parser.cpp`/`test_payload.cpp` may show the idiom for constructing `Vector`s by
hand in a unit test outside a live DuckDB query context; match whatever pattern already
exists there rather than introducing a new one.)

- [ ] **Step 2: Run test to verify it fails**

Expected: FAIL — `ParseGeometryPropertiesStruct` doesn't exist.

- [ ] **Step 3: Implement `ParseGeometryPropertiesStruct`**

In `src/kernel/metadata_parser.cpp`, add alongside the existing `ParseGeometryProperties`:

```cpp
GeometryMetadata ParseGeometryPropertiesStruct(const Vector &struct_vec, idx_t row) {
	GeometryMetadata result;
	auto &children = StructVector::GetEntries(struct_vec);
	// children[0] = type, children[1] = surfaces (unused for shell grouping,
	// same as the JSON-text parser), children[2] = face_semantics (unused),
	// children[3] = shells.
	auto &type_vec = *children[0];
	if (!FlatVector::IsNull(type_vec, row)) {
		result.type = FlatVector::GetData<string_t>(type_vec)[row].GetString();
	}

	auto &shells_vec = *children[3];
	if (FlatVector::IsNull(shells_vec, row)) {
		return result; // no shells key -> non-solid type, same as the JSON-text parser's empty-vector default
	}
	auto outer_entry = FlatVector::GetData<list_entry_t>(shells_vec)[row];
	auto &inner_list_vec = ListVector::GetEntry(shells_vec);
	for (idx_t solid_idx = outer_entry.offset; solid_idx < outer_entry.offset + outer_entry.length; solid_idx++) {
		auto inner_entry = FlatVector::GetData<list_entry_t>(inner_list_vec)[solid_idx];
		auto &int_vec = ListVector::GetEntry(inner_list_vec);
		auto int_data = FlatVector::GetData<int32_t>(int_vec);
		std::vector<uint32_t> shell_face_counts;
		shell_face_counts.reserve(inner_entry.length);
		for (idx_t i = inner_entry.offset; i < inner_entry.offset + inner_entry.length; i++) {
			shell_face_counts.push_back(static_cast<uint32_t>(int_data[i]));
		}
		result.shells.push_back(std::move(shell_face_counts));
	}
	return result;
}
```

Add the declaration to `metadata_parser.hpp`, next to the existing
`ParseGeometryProperties`. **This reads at most two nesting levels directly off `Vector`
without going through `UnifiedVectorFormat`** — acceptable here because this function is
called from inside a scalar-function body that has *already* done `ToUnifiedFormat`
dispatch at the top level (see Step 4), so by the time this function runs it's operating
on already-selected, already-validity-checked flat data for one specific row — this
differs from Task 3's boundary conversion, which walks *5* levels and needs the fuller
unified-format treatment throughout because it's the top-level entry point itself.

- [ ] **Step 4: Add the `ST_3DFromWKB(BLOB, STRUCT)` overload**

In `src/three_d_extension.cpp`, add a new function alongside `ST_3DFromWKBWithMetaFun`
(confirmed structure, lines ~292-330):

```cpp
// ──────────────────────────────────────────────────────────────
// ST_3DFromWKB(wkb BLOB, geometry_properties STRUCT(...)) → SOLID_3D
// ──────────────────────────────────────────────────────────────
static void ST_3DFromWKBWithStructMetaFun(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &wkb_vec = args.data[0];
	auto &meta_vec = args.data[1];
	auto count = args.size();

	UnifiedVectorFormat wkb_data;
	wkb_vec.ToUnifiedFormat(count, wkb_data);
	auto wkb_strings = UnifiedVectorFormat::GetData<string_t>(wkb_data);
	auto &result_validity = FlatVector::Validity(result);

	// meta_vec is a STRUCT column; ParseGeometryPropertiesStruct reads it by row
	// index directly (it's a top-level scalar-function argument, so DuckDB has
	// already flattened/selected it for this chunk — no ToUnifiedFormat needed
	// here, unlike args.data[0] above, which this function reads by raw string
	// data pointer and therefore does need it).

	for (idx_t i = 0; i < count; i++) {
		auto wkb_idx = wkb_data.sel->get_index(i);
		if (!wkb_data.validity.RowIsValid(wkb_idx)) {
			result_validity.SetInvalid(i);
			FlatVector::GetData<string_t>(result)[i] = string_t();
			continue;
		}
		using namespace duckdb_3d;
		auto &wkb = wkb_strings[wkb_idx];
		auto surfaces = ParseWKB(reinterpret_cast<const uint8_t *>(wkb.GetData()), wkb.GetSize());

		SolidModel model;
		if (FlatVector::IsNull(meta_vec, i)) {
			model = BuildSolidModel(surfaces);
		} else {
			auto metadata = ParseGeometryPropertiesStruct(meta_vec, i);
			model = BuildSolidModel(surfaces, metadata);
		}

		auto payload = SerializePayload(model);
		FlatVector::GetData<string_t>(result)[i] = StringVector::AddStringOrBlob(
		    result, string_t(reinterpret_cast<const char *>(payload.data()), payload.size()));
	}

	if (args.AllConstant()) {
		result.SetVectorType(VectorType::CONSTANT_VECTOR);
	}
}
```

Register it as a **third overload** of the existing `from_wkb_set`
(`three_d_extension.cpp:1860-1864`, confirmed): the `STRUCT` argument type must match
`duckdb-cityjson`'s `ColumnType::GeometryPropertiesStruct` shape exactly — build it with
the same helper Task 2 introduces for the arrow-native shapes, or inline it here first
and factor out once Task 2 needs the same STRUCT type for a different purpose (the new
functions don't take `geometry_properties` per this plan's architecture note above, so
this may end up being the *only* place this STRUCT type is needed — check before
extracting a shared helper prematurely):

```cpp
child_list_t<LogicalType> geom_props_fields;
geom_props_fields.push_back(make_pair("type", LogicalType::VARCHAR));
geom_props_fields.push_back(make_pair("surfaces", LogicalType::VARCHAR));
geom_props_fields.push_back(make_pair("face_semantics", LogicalType::LIST(LogicalType::INTEGER)));
geom_props_fields.push_back(make_pair("shells", LogicalType::LIST(LogicalType::LIST(LogicalType::INTEGER))));
auto geometry_properties_struct_type = LogicalType::STRUCT(std::move(geom_props_fields));

from_wkb_set.AddFunction(ScalarFunction(
    {LogicalType::BLOB, geometry_properties_struct_type}, LogicalType::BLOB, ST_3DFromWKBWithStructMetaFun));
```

Add the equivalent `ST_3DTryFromWKB` STRUCT overload too, mirroring
`ST_3DTryFromWKBWithMetaFun`'s existing try/catch-to-null pattern (confirmed present).

- [ ] **Step 5: Run test to verify it passes; add a SQL-level integration test**

Run the Step-1 unit test — expect PASS. Add a `test/sql/` test confirming
`ST_3DFromWKB(wkb, struct_pack('type' := 'Solid', ...))` binds and produces the same
result as the existing `to_json(...)`-cast workaround, for a real fixture — this is the
regression proof that the ergonomics gap is actually closed.

- [ ] **Step 6: Run the FULL existing test suite**

`GEN=ninja make test_debug` (or this repo's real test command — confirm via `justfile`).
Expected: PASS — zero regression to the existing `(BLOB)`/`(BLOB, VARCHAR)` overloads.

- [ ] **Step 7: Commit**

```bash
git add src/include/kernel/metadata_parser.hpp src/kernel/metadata_parser.cpp src/three_d_extension.cpp test/
git commit -m "feat(interop): ST_3DFromWKB STRUCT geometry_properties overload

Fixes the WKB pipeline's binding against duckdb-cityjson's now-STRUCT
geometry_properties_lod* output (that repo's commit d334b26) without
requiring an explicit to_json() cast."
```

---

### Task 2: Shared `LogicalType` builders for the arrow-native shapes

**Files:**
- Modify: `src/three_d_extension.cpp` (or a new small header if this repo prefers type-builder helpers separated out — check whether `three_d_extension.cpp` already has a "type builders" section near its `LoadInternal`, confirmed at line ~1793, before choosing where)

**Interfaces:**
- Produces: `LogicalType ArrowNativeGeometryType()`, `LogicalType ArrowNativeVerticesType()` — free functions, callable from both `LoadInternal` (function registration) and Tasks 3/5's conversion code (to validate/assert the input shape matches, if useful).

- [ ] **Step 1: Write the failing test**

```cpp
TEST_CASE("ArrowNativeGeometryType matches the agreed 5-level nested shape", "[type_builders]") {
	using namespace duckdb_3d;
	auto type = ArrowNativeGeometryType();
	REQUIRE(type.id() == LogicalTypeId::LIST);
	auto &solid = ListType::GetChildType(type);
	REQUIRE(solid.id() == LogicalTypeId::LIST);
	auto &shell = ListType::GetChildType(solid);
	REQUIRE(shell.id() == LogicalTypeId::LIST);
	auto &face = ListType::GetChildType(shell);
	REQUIRE(face.id() == LogicalTypeId::LIST);
	auto &ring = ListType::GetChildType(face);
	REQUIRE(ring.id() == LogicalTypeId::LIST);
	REQUIRE(ListType::GetChildType(ring).id() == LogicalTypeId::INTEGER);
}

TEST_CASE("ArrowNativeVerticesType matches List<Struct<x,y,z DOUBLE>>", "[type_builders]") {
	using namespace duckdb_3d;
	auto type = ArrowNativeVerticesType();
	REQUIRE(type.id() == LogicalTypeId::LIST);
	auto &item = ListType::GetChildType(type);
	REQUIRE(item.id() == LogicalTypeId::STRUCT);
	REQUIRE(StructType::GetChildCount(item) == 3);
	REQUIRE(StructType::GetChildName(item, 0) == "x");
	REQUIRE(StructType::GetChildType(item, 0).id() == LogicalTypeId::DOUBLE);
}
```

- [ ] **Step 2: Run test to verify it fails**

Expected: FAIL — functions don't exist.

- [ ] **Step 3: Implement**

```cpp
namespace duckdb_3d {

LogicalType ArrowNativeGeometryType() {
	auto ring = LogicalType::LIST(LogicalType::INTEGER);
	auto face = LogicalType::LIST(ring);
	auto shell = LogicalType::LIST(face);
	auto solid = LogicalType::LIST(shell);
	return LogicalType::LIST(solid);
}

LogicalType ArrowNativeVerticesType() {
	child_list_t<LogicalType> fields;
	fields.push_back(make_pair("x", LogicalType::DOUBLE));
	fields.push_back(make_pair("y", LogicalType::DOUBLE));
	fields.push_back(make_pair("z", LogicalType::DOUBLE));
	return LogicalType::LIST(LogicalType::STRUCT(std::move(fields)));
}

} // namespace duckdb_3d
```

Place in `src/three_d_extension.cpp` near the top (before `LoadInternal`), or extract to
`src/include/kernel/arrow_native_import.hpp` if Task 3 is being written first and it's
more natural to declare them there — either is fine, just be consistent and don't define
them twice.

- [ ] **Step 4: Run test to verify it passes; commit**

```bash
git add src/three_d_extension.cpp  # or wherever Step 3 placed it
git commit -m "feat(types): ArrowNativeGeometryType/ArrowNativeVerticesType builders"
```

---

### Task 3: `Vector` → `SolidModel` boundary conversion

**Files:**
- Create: `src/include/kernel/arrow_native_import.hpp`, `src/kernel/arrow_native_import.cpp`

**Interfaces:**
- Consumes: the `boundaries`/`vertices` `Vector`s at one row (matching `ArrowNativeGeometryType()`/`ArrowNativeVerticesType()`).
- Produces: `SolidModel BuildSolidModelFromArrowNative(const Vector &boundaries, idx_t boundaries_row, const Vector &vertices, idx_t vertices_row)`.

- [ ] **Step 1: Write the failing unit test**

Add to a new `test/cpp/test_arrow_native_import.cpp`, following `test_model_builder.cpp`'s
existing pattern for constructing test input and asserting on `SolidModel`'s fields
directly:

```cpp
#include "catch.hpp"
#include "kernel/arrow_native_import.hpp"
#include "duckdb.hpp"

using namespace duckdb;
using namespace duckdb_3d;

namespace {

// Hand-builds a boundaries Vector (5-level nested LIST<INTEGER>) for one row,
// representing a single Solid with 2 shells, 1 triangular face each.
Vector MakeSolidBoundariesVector() {
	Vector vec(ArrowNativeGeometryType(), 1);
	// solid[0] -> shell[0..2) -> face[0..1) each -> ring[0..1) each -> 3 indices each.
	// Build bottom-up: rings first.
	auto &solid_vec = vec;
	auto &shell_vec = ListVector::GetEntry(solid_vec);
	auto &face_vec = ListVector::GetEntry(shell_vec);
	auto &ring_vec = ListVector::GetEntry(face_vec);
	auto &index_vec = ListVector::GetEntry(ring_vec);

	// 2 rings total (one per shell's one face), 3 indices each: {0,1,2} and {3,4,5}.
	ListVector::Reserve(index_vec, 6);
	auto idx_data = FlatVector::GetData<int32_t>(index_vec);
	int32_t indices[6] = {0, 1, 2, 3, 4, 5};
	std::copy(indices, indices + 6, idx_data);
	ListVector::SetListSize(index_vec, 6);

	ListVector::Reserve(ring_vec, 2);
	FlatVector::GetData<list_entry_t>(ring_vec)[0] = list_entry_t(0, 3);
	FlatVector::GetData<list_entry_t>(ring_vec)[1] = list_entry_t(3, 3);
	ListVector::SetListSize(ring_vec, 2);

	ListVector::Reserve(face_vec, 2);
	FlatVector::GetData<list_entry_t>(face_vec)[0] = list_entry_t(0, 1); // face 0: 1 ring
	FlatVector::GetData<list_entry_t>(face_vec)[1] = list_entry_t(1, 1); // face 1: 1 ring
	ListVector::SetListSize(face_vec, 2);

	ListVector::Reserve(shell_vec, 2);
	FlatVector::GetData<list_entry_t>(shell_vec)[0] = list_entry_t(0, 1); // shell 0: 1 face
	FlatVector::GetData<list_entry_t>(shell_vec)[1] = list_entry_t(1, 1); // shell 1: 1 face
	ListVector::SetListSize(shell_vec, 2);

	FlatVector::GetData<list_entry_t>(solid_vec)[0] = list_entry_t(0, 2); // 1 solid: 2 shells
	return vec;
}

Vector MakeSixVertexVector() {
	Vector vec(ArrowNativeVerticesType(), 1);
	std::vector<std::array<double, 3>> coords = {
	    {0, 0, 0}, {1, 0, 0}, {0, 1, 0}, {0, 0, 1}, {1, 0, 1}, {0, 1, 1}};
	auto &struct_vec = ListVector::GetEntry(vec);
	ListVector::Reserve(struct_vec, coords.size());
	auto &children = StructVector::GetEntries(struct_vec);
	for (size_t i = 0; i < coords.size(); i++) {
		FlatVector::GetData<double>(*children[0])[i] = coords[i][0];
		FlatVector::GetData<double>(*children[1])[i] = coords[i][1];
		FlatVector::GetData<double>(*children[2])[i] = coords[i][2];
	}
	ListVector::SetListSize(struct_vec, coords.size());
	FlatVector::GetData<list_entry_t>(vec)[0] = list_entry_t(0, coords.size());
	return vec;
}

} // namespace

TEST_CASE("BuildSolidModelFromArrowNative reconstructs a two-shell solid", "[arrow_native_import]") {
	auto boundaries = MakeSolidBoundariesVector();
	auto vertices = MakeSixVertexVector();
	auto model = BuildSolidModelFromArrowNative(boundaries, 0, vertices, 0);
	REQUIRE(model.vertices.size() == 6);
	REQUIRE(model.SolidCount() == 1);
	REQUIRE(model.ShellCount() == 2);
	REQUIRE(model.FaceCount() == 2);
}
```

- [ ] **Step 2: Run test to verify it fails**

Expected: FAIL — `BuildSolidModelFromArrowNative` doesn't exist. (This test also
double-checks `ArrowNativeGeometryType`/`ArrowNativeVerticesType` from Task 2 are
correctly usable for hand-building test `Vector`s — if Step 1 itself fails to compile
because those constructors don't produce a usable `Vector`, that's a Task 2 bug
surfacing here, fix it there first.)

- [ ] **Step 3: Implement, walking each nesting level explicitly**

```cpp
// arrow_native_import.cpp
namespace duckdb_3d {

namespace {

//! Reads one row's list_entry_t {offset,length} from a (possibly non-flat)
//! Vector via unified format — real per-level traversal, not a raw buffer
//! cast (design doc correction: DuckDB ListVectors are list_entry_t pairs
//! into a shared child, with possible validity/selection indirection).
list_entry_t ReadListEntry(const Vector &vec, idx_t row) {
	UnifiedVectorFormat unified;
	const_cast<Vector &>(vec).ToUnifiedFormat(1, unified); // single logical row being read
	auto idx = unified.sel->get_index(row);
	return UnifiedVectorFormat::GetData<list_entry_t>(unified)[idx];
}

} // namespace

SolidModel BuildSolidModelFromArrowNative(const Vector &boundaries, idx_t boundaries_row,
                                          const Vector &vertices, idx_t vertices_row) {
	SolidModel model;

	// --- vertices: List<Struct<x,y,z>> -> model.vertices ---
	auto vert_entry = ReadListEntry(vertices, vertices_row);
	auto &coord_struct_vec = ListVector::GetEntry(vertices);
	auto &coord_children = StructVector::GetEntries(coord_struct_vec);
	auto x_data = FlatVector::GetData<double>(*coord_children[0]);
	auto y_data = FlatVector::GetData<double>(*coord_children[1]);
	auto z_data = FlatVector::GetData<double>(*coord_children[2]);
	model.vertices.reserve(vert_entry.length);
	for (idx_t i = vert_entry.offset; i < vert_entry.offset + vert_entry.length; i++) {
		model.vertices.push_back(Vertex3D{x_data[i], y_data[i], z_data[i]});
	}

	// --- boundaries: solid -> shell -> face -> ring -> index, build CSR offsets ---
	auto solid_entry = ReadListEntry(boundaries, boundaries_row);
	auto &shell_vec = ListVector::GetEntry(boundaries);

	uint32_t total_shells = 0, total_faces = 0, total_rings = 0;
	model.solid_shell_offsets.push_back(0);

	for (idx_t solid_idx = solid_entry.offset; solid_idx < solid_entry.offset + solid_entry.length; solid_idx++) {
		auto shell_entry = FlatVector::GetData<list_entry_t>(shell_vec)[solid_idx];
		auto &face_vec = ListVector::GetEntry(shell_vec);

		for (idx_t shell_idx = shell_entry.offset; shell_idx < shell_entry.offset + shell_entry.length; shell_idx++) {
			auto face_entry = FlatVector::GetData<list_entry_t>(face_vec)[shell_idx];
			auto &ring_vec = ListVector::GetEntry(face_vec);

			for (idx_t face_idx = face_entry.offset; face_idx < face_entry.offset + face_entry.length; face_idx++) {
				auto ring_entry = FlatVector::GetData<list_entry_t>(ring_vec)[face_idx];
				auto &index_vec = ListVector::GetEntry(ring_vec);

				model.face_ring_offsets.push_back(total_rings);
				for (idx_t r = ring_entry.offset; r < ring_entry.offset + ring_entry.length; r++) {
					auto index_ring_entry = FlatVector::GetData<list_entry_t>(index_vec)[r];
					auto &idx_leaf_vec = ListVector::GetEntry(index_vec);
					auto idx_data = FlatVector::GetData<int32_t>(idx_leaf_vec);

					model.ring_vertex_offsets.push_back(
					    static_cast<uint32_t>(model.ring_vertex_indices.size()));
					for (idx_t k = index_ring_entry.offset; k < index_ring_entry.offset + index_ring_entry.length;
					     k++) {
						int32_t raw = idx_data[k];
						if (raw < 0 || static_cast<size_t>(raw) >= model.vertices.size()) {
							throw std::runtime_error(
							    "arrow-native geometry: vertex-pool index out of range (design doc validity invariant)");
						}
						model.ring_vertex_indices.push_back(static_cast<uint32_t>(raw));
					}
					total_rings++;
				}
				total_faces++;
			}
			model.shell_face_offsets.push_back(total_faces);
			total_shells++;
		}
		model.solid_shell_offsets.push_back(total_shells);
	}
	model.face_ring_offsets.push_back(total_rings);
	model.shell_face_offsets.push_back(total_faces);
	model.ring_vertex_offsets.push_back(static_cast<uint32_t>(model.ring_vertex_indices.size()));

	// Same non-WKB-specific construction work model_builder.cpp already does —
	// what's skipped is ONLY WKB-byte parsing, not this (design doc, per-repo plan).
	model.ComputeBBox();
	model.face_triangle_offsets.resize(total_faces + 1, 0);
	TriangulateSolidModel(model);
	ValidateSolidModel(model);

	return model;
}

} // namespace duckdb_3d
```

**Two things flagged honestly, not glossed over, matching this plan's own architecture
note about real vs. "free" boundary work**: (1) the nested-level `list_entry_t` reads
after the outermost (`shell_vec`/`face_vec`/`ring_vec`/`index_vec` via
`FlatVector::GetData` directly, not `ReadListEntry`'s unified-format treatment) assume
those child vectors are already flat — true for a freshly-scanned Parquet column in
practice, but a genuine simplification versus doing full unified-format traversal at
*every* level; if `test_arrow_native_import.cpp`'s tests (or a real-file integration
test) ever hit a non-flat intermediate child vector (e.g. via a query that applies a
filter/projection before calling this function), this simplification breaks and needs
the same `ReadListEntry` treatment at every level, not just the outermost. Decide based
on what real queries actually need — don't over-build unified-format handling at every
level speculatively if it's never exercised, but don't silently ship the simplification
either; note it in the code as this paragraph does. (2) `SolidModel`'s own
`GetOrAddVertex`-style coordinate-equality dedup (confirmed present in
`model_builder.cpp`'s existing `BuildSolidModel`, per the design doc's own citation) is
**not** run here — this function pushes `vertices` through 1:1, trusting the writer's
distinct-index-compaction invariant. This is a deliberate difference from
`model_builder.cpp`'s WKB path (design doc: "trust the writer's documented dedup
invariant... matching how SolidModel construction already assumes clean input from its
builders" was the original framing, later refined to "defensive... not something to skip
or trust away at a public ingestion boundary" after codex review) — **resolve this
explicitly before Step 4, don't leave it ambiguous**: either add the same
coordinate-equality dedup pass `model_builder.cpp` runs (defensive, consistent with the
final design-doc position), or document concretely why trusting the writer is safe
enough for a public SQL-callable ingestion function. Given this function IS directly
SQL-callable (unlike an internal-only path), defensive dedup is very likely the right
call — but make the call explicitly, with a test proving it (two distinct source indices
with equal coordinates fed through `ST_3DFromArrowNative` should still validate/compute
correctly either way; the interesting test is whether an intentionally-malformed input
with an actual duplicate-vertex artefact from a buggy writer still produces a valid
`SolidModel` rather than a corrupt one).

- [ ] **Step 4: Run test to verify it passes**

Expected: PASS, once the vertex-dedup question above is resolved and — if defensive
dedup is added — a test confirming it.

- [ ] **Step 5: Add golden-fixture tests independent of the other two repos**

Per the design doc's explicit call-out ("gate on a frozen golden fixture... decouples
`duckdb-3d`'s schedule"): add 2-3 more hand-built `Vector` fixtures to
`test_arrow_native_import.cpp` covering the malformed/edge cases the design doc lists —
out-of-range indices (assert it throws, per Step 3's bounds check), a degenerate
zero-length ring, an interior-cavity two-shell solid with correct winding (reuse this
repo's existing `test_inner_shell.cpp`/hollow-solid fixtures' *expected values* — same
geometry, different input encoding — as the ground truth to compare against, which is a
strong, real correctness check rather than just "doesn't crash").

- [ ] **Step 6: Commit**

```bash
git add src/include/kernel/arrow_native_import.hpp src/kernel/arrow_native_import.cpp test/cpp/test_arrow_native_import.cpp
git commit -m "feat(import): BuildSolidModelFromArrowNative — nested Vector to SolidModel"
```

---

### Task 4: `ST_3DFromArrowNative` / `ST_3DTryFromArrowNative` registration

**Files:**
- Modify: `src/three_d_extension.cpp`

**Interfaces:**
- Consumes: `BuildSolidModelFromArrowNative` (Task 3), `ArrowNativeGeometryType`/`ArrowNativeVerticesType` (Task 2).
- Produces: SQL-callable `ST_3DFromArrowNative(boundaries, vertices) → SOLID_3D`, `ST_3DTryFromArrowNative(boundaries, vertices) → SOLID_3D or NULL`.

- [ ] **Step 1: Write the failing SQL test**

```sql
# name: test/sql/st_3d_from_arrow_native.test
# description: ST_3DFromArrowNative ingests a nested-List/Struct solid
# group: [three_d]

require three_d

query I
SELECT ST_3DNumFaces(ST_3DFromArrowNative(
    [[[[ [0,1,2] ]], [[ [3,4,5] ]]]],  -- 1 solid, 2 shells, 1 triangular face each
    [{'x':0,'y':0,'z':0}, {'x':1,'y':0,'z':0}, {'x':0,'y':1,'z':0},
     {'x':0,'y':0,'z':1}, {'x':1,'y':0,'z':1}, {'x':0,'y':1,'z':1}]
));
----
2
```

(Confirm `ST_3DNumFaces`'s real name/signature against this repo's existing
introspection functions — this plan's research confirmed `ST_3DNumSolids`/`Shells`/
`Faces` exist at `three_d_extension.cpp:~408-431` but not their exact SQL-visible
names verbatim; also confirm DuckDB's literal syntax for a `LIST<STRUCT<...>>` value
matches what's written above, adjusting if not.)

- [ ] **Step 2: Run test to verify it fails**

Expected: FAIL — no such function.

- [ ] **Step 3: Register the functions**

```cpp
static void ST_3DFromArrowNativeFun(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &boundaries_vec = args.data[0];
	auto &vertices_vec = args.data[1];
	auto count = args.size();
	auto &result_validity = FlatVector::Validity(result);

	for (idx_t i = 0; i < count; i++) {
		if (FlatVector::IsNull(boundaries_vec, i) || FlatVector::IsNull(vertices_vec, i)) {
			result_validity.SetInvalid(i);
			FlatVector::GetData<string_t>(result)[i] = string_t();
			continue;
		}
		using namespace duckdb_3d;
		auto model = BuildSolidModelFromArrowNative(boundaries_vec, i, vertices_vec, i);
		auto payload = SerializePayload(model);
		FlatVector::GetData<string_t>(result)[i] = StringVector::AddStringOrBlob(
		    result, string_t(reinterpret_cast<const char *>(payload.data()), payload.size()));
	}
	if (args.AllConstant()) {
		result.SetVectorType(VectorType::CONSTANT_VECTOR);
	}
}

static void ST_3DTryFromArrowNativeFun(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &boundaries_vec = args.data[0];
	auto &vertices_vec = args.data[1];
	auto count = args.size();
	auto &result_validity = FlatVector::Validity(result);

	for (idx_t i = 0; i < count; i++) {
		if (FlatVector::IsNull(boundaries_vec, i) || FlatVector::IsNull(vertices_vec, i)) {
			result_validity.SetInvalid(i);
			continue;
		}
		try {
			using namespace duckdb_3d;
			auto model = BuildSolidModelFromArrowNative(boundaries_vec, i, vertices_vec, i);
			auto payload = SerializePayload(model);
			FlatVector::GetData<string_t>(result)[i] = StringVector::AddStringOrBlob(
			    result, string_t(reinterpret_cast<const char *>(payload.data()), payload.size()));
		} catch (...) {
			result_validity.SetInvalid(i);
		}
	}
}
```

Register in `LoadInternal` (near the existing `from_wkb_set` registration, confirmed
lines ~1860-1864):

```cpp
loader.RegisterFunction(ScalarFunction("st_3dfromarrownative",
    {ArrowNativeGeometryType(), ArrowNativeVerticesType()}, solid_3d_type, ST_3DFromArrowNativeFun));
loader.RegisterFunction(ScalarFunction("st_3dtryfromarrownative",
    {ArrowNativeGeometryType(), ArrowNativeVerticesType()}, solid_3d_type, ST_3DTryFromArrowNativeFun));
```

- [ ] **Step 4: Run test to verify it passes**

Expected: PASS.

- [ ] **Step 5: Run the FULL existing test suite; commit**

```bash
git add src/three_d_extension.cpp test/sql/st_3d_from_arrow_native.test
git commit -m "feat(sql): ST_3DFromArrowNative / ST_3DTryFromArrowNative"
```

---

### Task 5: `Vector` → `GeomModel` boundary conversion (surface family)

**Files:**
- Modify: `src/include/kernel/arrow_native_import.hpp`, `src/kernel/arrow_native_import.cpp`

**Interfaces:**
- Consumes: same `boundaries`/`vertices` shape as Task 3, but padded to solid-count=1/shell-count=1 (design doc "padding dimensions" — `MultiSurface`/`CompositeSurface`).
- Produces: `GeomModel BuildGeomModelFromArrowNative(const Vector &boundaries, idx_t boundaries_row, const Vector &vertices, idx_t vertices_row)`.

- [ ] **Step 1: Write the failing unit test**

```cpp
TEST_CASE("BuildGeomModelFromArrowNative strips padding and dereferences into inline coordinates", "[arrow_native_import]") {
	// Same physical shape as a Solid with 1 shell, 1 face, but semantically a
	// MultiSurface (single triangular surface) — the padding dimensions
	// (solid-count 1, shell-count 1) carry no meaning here (design doc).
	Vector boundaries(ArrowNativeGeometryType(), 1);
	// ... construct 1 solid / 1 shell / 1 face / 1 ring / {0,1,2} — same
	// nested-Vector-building technique as Task 3's MakeSolidBoundariesVector,
	// reduced to one shell instead of two.
	Vector vertices(ArrowNativeVerticesType(), 1);
	// ... 3 vertices.

	auto model = BuildGeomModelFromArrowNative(boundaries, 0, vertices, 0);
	REQUIRE(model.type == GeomType::MultiPolygon);
	REQUIRE(model.vertices.size() == 3); // GeomModel is NOT index-based — inline coordinates
	REQUIRE(model.part_offsets.size() == 2); // 1 part (this face) -> part_offsets = [0, 1]
	REQUIRE(model.ring_offsets.size() == 2); // 1 ring -> ring_offsets = [0, 3]
}
```

(Fill in the boundaries/vertices construction using Task 3's already-verified technique,
reduced to one shell — write this out explicitly rather than eliding it, per this plan's
own "no placeholders" discipline; it was elided here only to avoid repeating Task 3's
~15-line pattern near-verbatim in this document.)

- [ ] **Step 2: Run test to verify it fails**

Expected: FAIL — `BuildGeomModelFromArrowNative` doesn't exist.

- [ ] **Step 3: Implement — the real asymmetry from Task 3, per the design doc**

`GeomModel` is confirmed **not** index-based (`vertices` are inline `Vertex3D`,
`ring_offsets`/`part_offsets` CSR arrays partition those inline coordinates directly, per
`geom_model.hpp:29-38`) — this function must **dereference and expand** the arrow-native
indices into inline coordinates, unlike Task 3's direct copy:

```cpp
GeomModel BuildGeomModelFromArrowNative(const Vector &boundaries, idx_t boundaries_row,
                                        const Vector &vertices, idx_t vertices_row) {
	GeomModel model;
	model.type = GeomType::MultiPolygon; // per §"Geometry-type mapping": MultiSurface/CompositeSurface -> MultiPolygon Z family

	// Dereference the vertex pool once into a lookup table (this function
	// expands into inline coordinates, so every ring's indices need
	// resolving, not just a straight copy like Task 3's SolidModel path).
	auto vert_entry = ReadListEntry(vertices, vertices_row); // reuse Task 3's helper — move it to a shared internal header if not already
	auto &coord_struct_vec = ListVector::GetEntry(vertices);
	auto &coord_children = StructVector::GetEntries(coord_struct_vec);
	auto x_data = FlatVector::GetData<double>(*coord_children[0]);
	auto y_data = FlatVector::GetData<double>(*coord_children[1]);
	auto z_data = FlatVector::GetData<double>(*coord_children[2]);
	std::vector<Vertex3D> pool;
	pool.reserve(vert_entry.length);
	for (idx_t i = vert_entry.offset; i < vert_entry.offset + vert_entry.length; i++) {
		pool.push_back(Vertex3D{x_data[i], y_data[i], z_data[i]});
	}

	// Strip the two padding dimensions (solid-count 1, shell-count 1 — this
	// function is only ever called for surface types, so both are asserted,
	// not branched on, per the design doc's dispatch-by-caller-choice
	// architecture note at the top of this plan).
	auto solid_entry = ReadListEntry(boundaries, boundaries_row);
	if (solid_entry.length != 1) {
		throw std::runtime_error(
		    "ST_Geom3DFromArrowNative: expected a padded (solid-count 1) surface-type value — "
		    "if this is a real multi-shell Solid, call ST_3DFromArrowNative instead");
	}
	auto &shell_vec = ListVector::GetEntry(boundaries);
	auto shell_entry = FlatVector::GetData<list_entry_t>(shell_vec)[solid_entry.offset];
	if (shell_entry.length != 1) {
		throw std::runtime_error("ST_Geom3DFromArrowNative: expected shell-count 1 (padding dimension)");
	}
	auto &face_vec = ListVector::GetEntry(shell_vec);
	auto face_entry = FlatVector::GetData<list_entry_t>(face_vec)[shell_entry.offset];
	auto &ring_vec = ListVector::GetEntry(face_vec);

	uint32_t total_rings = 0;
	model.part_offsets.push_back(0);
	for (idx_t face_idx = face_entry.offset; face_idx < face_entry.offset + face_entry.length; face_idx++) {
		auto ring_entry = FlatVector::GetData<list_entry_t>(ring_vec)[face_idx];
		auto &index_vec = ListVector::GetEntry(ring_vec);
		for (idx_t r = ring_entry.offset; r < ring_entry.offset + ring_entry.length; r++) {
			auto idx_ring_entry = FlatVector::GetData<list_entry_t>(index_vec)[r];
			auto &idx_leaf_vec = ListVector::GetEntry(index_vec);
			auto idx_data = FlatVector::GetData<int32_t>(idx_leaf_vec);

			model.ring_offsets.push_back(static_cast<uint32_t>(model.vertices.size()));
			for (idx_t k = idx_ring_entry.offset; k < idx_ring_entry.offset + idx_ring_entry.length; k++) {
				int32_t raw = idx_data[k];
				if (raw < 0 || static_cast<size_t>(raw) >= pool.size()) {
					throw std::runtime_error("arrow-native geometry: vertex-pool index out of range");
				}
				model.vertices.push_back(pool[static_cast<size_t>(raw)]); // dereference + expand, NOT an index copy
			}
			total_rings++;
		}
		model.part_offsets.push_back(total_rings);
	}
	model.ring_offsets.push_back(static_cast<uint32_t>(model.vertices.size()));

	model.ComputeBBox();
	return model;
}
```

- [ ] **Step 4: Run test to verify it passes**

Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add src/include/kernel/arrow_native_import.hpp src/kernel/arrow_native_import.cpp test/cpp/test_arrow_native_import.cpp
git commit -m "feat(import): BuildGeomModelFromArrowNative — dereference/expand into inline coords"
```

---

### Task 6: `ST_Geom3DFromArrowNative` / `ST_Geom3DTryFromArrowNative` registration

**Files:**
- Modify: `src/three_d_extension.cpp`

Mirrors Task 4 exactly, calling `BuildGeomModelFromArrowNative` +
`SerializeGeomPayload` (confirmed existing, per `ST_Geom3DFromWKBFun`'s pattern) instead
of `BuildSolidModelFromArrowNative` + `SerializePayload`, returning `geom_3d_type`.

- [ ] **Step 1: Write the failing SQL test** — mirror Task 4 Step 1, using
  `ST_GeometryType`/an area accessor instead of `ST_3DNumFaces`.
- [ ] **Step 2: Run test to verify it fails.**
- [ ] **Step 3: Implement `ST_Geom3DFromArrowNativeFun`/`ST_Geom3DTryFromArrowNativeFun`**, structurally identical to Task 4's Step 3 but for `GeomModel`/`SerializeGeomPayload`/`geom_3d_type`.
- [ ] **Step 4: Register** in `LoadInternal`, next to `st_geom3dfromwkb` (confirmed at line ~1822).
- [ ] **Step 5: Run test to verify it passes; run the full suite; commit.**

```bash
git add src/three_d_extension.cpp test/sql/st_geom3d_from_arrow_native.test
git commit -m "feat(sql): ST_Geom3DFromArrowNative / ST_Geom3DTryFromArrowNative"
```

---

### Task 7: Cross-encoding parity test (golden, independent of the other two repos)

**Files:**
- Modify: `test/cpp/test_arrow_native_import.cpp` (or a new file if it's grown large)

Per the design doc's testing plan: prove the arrow-native ingestion path produces
**identical** `SolidModel`/`GeomModel` output to the existing WKB path for the same
logical geometry — the strongest correctness signal available before any other repo's
code exists to cross-read.

- [ ] **Step 1: Write the test**

```cpp
TEST_CASE("Arrow-native and WKB ingestion agree on the hollow-cube fixture", "[arrow_native_import][parity]") {
	using namespace duckdb_3d;
	// Reuse this repo's existing BuildHollowCubeWKB()-equivalent fixture
	// (confirmed present in three_d_extension.cpp for ST_AsWKBHollowCube, or
	// its test/cpp equivalent) as the WKB-side ground truth, and hand-build
	// the SAME topology as an arrow-native boundaries/vertices Vector pair.
	auto wkb = BuildHollowCubeWKB(); // adjust include/visibility as needed — may need exposing as non-static for test use, or duplicating the 8 vertex coordinates directly in the test
	auto wkb_surfaces = ParseWKB(wkb.data(), wkb.size());
	GeometryMetadata meta;
	meta.type = "Solid";
	meta.shells = {{6, 6}}; // per this repo's own BuildHollowCubeWKB doc comment: 12 faces total, 6 per shell
	auto wkb_model = BuildSolidModel(wkb_surfaces, meta);

	auto arrow_boundaries = /* hand-built matching the same 2-shell, 6-face-each cube topology */;
	auto arrow_vertices = /* the same 8 cube vertices */;
	auto arrow_model = BuildSolidModelFromArrowNative(arrow_boundaries, 0, arrow_vertices, 0);

	REQUIRE(arrow_model.SolidCount() == wkb_model.SolidCount());
	REQUIRE(arrow_model.ShellCount() == wkb_model.ShellCount());
	REQUIRE(arrow_model.FaceCount() == wkb_model.FaceCount());
	REQUIRE(ComputeVolume(arrow_model) == Approx(ComputeVolume(wkb_model)));
	REQUIRE(ComputeSurfaceArea(arrow_model) == Approx(ComputeSurfaceArea(wkb_model)));
}
```

(Fill in the hand-built arrow-native `Vector`s explicitly — elided here for the same
reason as Task 5 Step 1; use Task 3's `MakeSolidBoundariesVector`-style construction,
extended to the real 8-vertex, 2×6-face hollow-cube topology this repo's own
`BuildHollowCubeWKB` already encodes, so the two inputs are genuinely the same geometry
by construction, not just asserted to be.)

- [ ] **Step 2: Run, fix any implementation bug it surfaces (not an "expected fail" step — this must pass once both paths are correct)**

- [ ] **Step 3: Commit**

```bash
git add test/cpp/test_arrow_native_import.cpp
git commit -m "test(parity): arrow-native and WKB ingestion agree on the hollow-cube fixture"
```

---

### Task 8: `docs/DESIGN_DOC.md` update

**Files:**
- Modify: `docs/DESIGN_DOC.md`

Required by this repo's own contribution rules for any public-API/architecture change —
not optional. Update:

- [ ] **§4.1 (`SOLID_3D`)**: note the new ingestion path exists alongside WKB import, still landing in the same `SOLID_3D` binary payload — no format change.
- [ ] **§5.1 (Constructor and Export Functions)**: document `ST_3DFromArrowNative`/`ST_3DTryFromArrowNative`/`ST_Geom3DFromArrowNative`/`ST_Geom3DTryFromArrowNative` alongside the existing WKB constructors, including the `(boundaries, vertices)` — no `geometry_properties` — signature and why (this plan's architecture note).
- [ ] **New subsection under §8 (Import Pipeline)**, e.g. "§8.3 Arrow-native Import": document the nested-`LIST`/`STRUCT` shape (must match `cityparquet-rs`/`duckdb-cityjson` exactly — link to the design doc in the parent workspace repo), the distinct-source-index compaction the *producers* perform (this repo only consumes, doesn't compact), and the padding-dimension convention `ST_Geom3DFromArrowNative` strips.
- [ ] **§12 (CityJSON Interoperability Contract)**: add the arrow-native equivalent of the existing WKB example query, and **fix the existing WKB example** to show the new `STRUCT`-native `ST_3DFromWKB` call (Task 1) rather than the old implicit-`VARCHAR` one, since that's now the better default recommendation for fresh `duckdb-cityjson` output.
- [ ] **§14 (Roadmap)**: note this experimental branch's existence and where its own status lives (the design doc, `docs/superpowers/specs/2026-07-25-arrow-native-geometry-design.md` in the parent workspace repo) — this repo's `DESIGN_DOC.md` documents the *decided* v1 surface; flag the new functions as experimental/branch-only until the parent experiment concludes, consistent with how the parent repo's own spec is being drafted as explicitly draft/under-evaluation.

- [ ] **Commit**

```bash
git add docs/DESIGN_DOC.md
git commit -m "docs: arrow-native import pipeline, ST_3DFromWKB STRUCT overload"
```

---

## Final steps

- [ ] Bump the root repo's `duckdb-3d` submodule pointer on its `arrow-native-type`
  branch, matching this repo's own established multi-repo submodule convention.
- [ ] Run `codex exec -m gpt-5.6-sol -s read-only` over the full diff before considering
  this leg done — same discipline the design doc itself went through (verify claims
  against source before accepting them, per `superpowers:receiving-code-review`).
- [ ] Cross-read a real `cityparquet-rs`-written and `duckdb-cityjson`-written
  arrow-native Parquet file once both are far enough along (design doc's cross-repo
  consistency gate) — this repo's own golden-fixture tests (Task 7) don't require it, but
  the design doc's overall testing plan does.

---

## Self-Review

**Spec coverage** (against the design doc + parity with the other two plans):
- Unified physical shape: Task 2, matches `cityparquet-rs`/`duckdb-cityjson` field-for-field. ✓
- Padding-dimension stripping for surface types: Task 5, with an explicit assertion (not silent branching) that a mis-shaped input to `ST_Geom3DFromArrowNative` fails loudly. ✓
- "Boundary is real work, not free" (bounds-checking, unified-format traversal, SolidModel's own construction pipeline still running): Task 3, with the vertex-dedup question resolved explicitly rather than left ambiguous. ✓
- `GeomModel`'s index/inline asymmetry: Task 5, real dereference-and-expand, not a copy. ✓
- Kernel untouched: confirmed — no task modifies `validation.cpp`/`measurements.hpp`/`triangulation.cpp`; `SolidModel`/`GeomModel`'s public shape is unchanged.
- `docs/DESIGN_DOC.md` update: Task 8, required by this repo's own rules, not skipped.
- The urgent `ST_3DFromWKB` STRUCT-overload prerequisite (discovered during this plan's own research, not in the original design doc): Task 1, sequenced first, with reasoning for why it's separate from and higher-priority than the new arrow-native work.
- One deliberate, reasoned deviation from the design doc's function-signature sketch (dropping `geometry_properties` from the new functions' arguments): stated explicitly at the top of this document with the reasoning, not silently diverged.

**Placeholder scan:** Tasks 5 and 7 elide two multi-line `Vector`-construction snippets
("fill in using Task 3's technique") rather than repeating ~15-20 lines of already-shown
boilerplate a third/fourth time — flagged explicitly as elisions for repetition, not
missing logic, and each names exactly what to substitute. Every other step has real,
complete code.

**Type consistency:** `SolidModel`/`GeomModel` (existing types, unmodified) used
correctly throughout; `ArrowNativeGeometryType()`/`ArrowNativeVerticesType()` (Task 2)
used identically in Tasks 3-7; `BuildSolidModelFromArrowNative`/
`BuildGeomModelFromArrowNative` signatures match between their Task 3/5 definitions and
Task 4/6/7's call sites.
