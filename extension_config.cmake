# This file is included by DuckDB's build system. It specifies which extension to load

# Extension from this repo
duckdb_extension_load(three_d
    SOURCE_DIR ${CMAKE_CURRENT_LIST_DIR}
    LOAD_TESTS
)

# json registers the JSON type, which CityParquet's geometry_properties_lod*.surfaces
# carries; linking it lets the SQL tests bind that shape without staging an extension.
duckdb_extension_load(json)
