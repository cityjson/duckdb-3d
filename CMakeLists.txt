cmake_minimum_required(VERSION 3.5)

# Set extension name here
set(TARGET_NAME three_d)

set(EXTENSION_NAME ${TARGET_NAME}_extension)
set(LOADABLE_EXTENSION_NAME ${TARGET_NAME}_loadable_extension)

project(${TARGET_NAME})
include_directories(src/include)

set(EXTENSION_SOURCES
    src/three_d_extension.cpp
    src/kernel/solid_model.cpp
    src/kernel/payload.cpp
    src/kernel/wkb_parser.cpp
    src/kernel/model_builder.cpp
    src/kernel/wkb_export.cpp
    src/kernel/validation.cpp
    src/kernel/triangulation.cpp
    src/kernel/measurements.cpp
    src/kernel/metadata_parser.cpp
    src/kernel/geom_model.cpp
    src/kernel/geom_wkb_parser.cpp
    src/kernel/geom_payload.cpp
    src/kernel/geom_distance.cpp
    src/kernel/geom_construct.cpp
    src/kernel/geom_analysis.cpp
    src/kernel/geom_serialize.cpp)

build_static_extension(${TARGET_NAME} ${EXTENSION_SOURCES})
build_loadable_extension(${TARGET_NAME} " " ${EXTENSION_SOURCES})

install(
  TARGETS ${EXTENSION_NAME}
  EXPORT "${DUCKDB_EXPORT_SET}"
  LIBRARY DESTINATION "${INSTALL_LIB_DIR}"
  ARCHIVE DESTINATION "${INSTALL_LIB_DIR}")
