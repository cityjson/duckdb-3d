#define DUCKDB_EXTENSION_MAIN

#include "three_d_extension.hpp"
#include "duckdb.hpp"
#include "functions/three_d_functions.hpp"

namespace duckdb {

// ──────────────────────────────────────────────────────────────
// Extension registration
// ──────────────────────────────────────────────────────────────
static void LoadInternal(ExtensionLoader &loader) {
	// Register SOLID_3D type (alias over BLOB)
	auto solid_3d_type = LogicalType(LogicalTypeId::BLOB);
	solid_3d_type.SetAlias("SOLID_3D");
	loader.RegisterType("SOLID_3D", solid_3d_type);

	// Register GEOM_3D type: a general 3D geometry (point/line/polygon/multi/
	// polyhedral-surface), also an alias over BLOB with its own payload (§16.2).
	auto geom_3d_type = LogicalType(LogicalTypeId::BLOB);
	geom_3d_type.SetAlias("GEOM_3D");
	loader.RegisterType("GEOM_3D", geom_3d_type);

	RegisterFixtureFunctions(loader);
	RegisterSolidIOFunctions(loader, solid_3d_type);
	RegisterSolidAccessorFunctions(loader, solid_3d_type, geom_3d_type);
	RegisterGeomAccessorFunctions(loader, solid_3d_type, geom_3d_type);
	RegisterDistanceFunctions(loader, geom_3d_type);
	RegisterTransformFunctions(loader, solid_3d_type, geom_3d_type);
	RegisterArrowNativeFunctions(loader, solid_3d_type, geom_3d_type);
}

void ThreeDExtension::Load(ExtensionLoader &loader) {
	LoadInternal(loader);
}

std::string ThreeDExtension::Name() {
	return "three_d";
}

std::string ThreeDExtension::Version() const {
#ifdef EXT_VERSION_THREE_D
	return EXT_VERSION_THREE_D;
#else
	return "";
#endif
}

} // namespace duckdb

extern "C" {

DUCKDB_CPP_EXTENSION_ENTRY(three_d, loader) {
	duckdb::LoadInternal(loader);
}
}
