cmake_minimum_required(VERSION 3.15)

# Set extension name here
set(TARGET_NAME three_d)

set(EXTENSION_NAME ${TARGET_NAME}_extension)
set(LOADABLE_EXTENSION_NAME ${TARGET_NAME}_loadable_extension)

project(${TARGET_NAME})

# The kernel uses C++17 features (e.g. if-init statements). Declare the standard
# explicitly so both the normal build and the clang-tidy pass parse as C++17.
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include_directories(src/include)

set(EXTENSION_SOURCES
    src/three_d_extension.cpp
    src/functions/fixtures.cpp
    src/functions/solid_io.cpp
    src/functions/solid_accessors.cpp
    src/functions/geom_accessors.cpp
    src/functions/distance.cpp
    src/functions/transforms.cpp
    src/functions/arrow_native.cpp
    src/functions/struct_metadata.cpp
    src/kernel/solid_model.cpp
    src/kernel/payload.cpp
    src/kernel/wkb_parser.cpp
    src/kernel/model_builder.cpp
    src/kernel/wkb_export.cpp
    src/kernel/geometry_math.cpp
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
    src/kernel/geom_serialize.cpp
    src/kernel/crs_transform.cpp
    src/kernel/arrow_native_import.cpp)

# PROJ backs ST_3DTransform (kernel/crs_transform.cpp). Under the vcpkg toolchain
# the vcpkg copy is found first; on a bare macOS dev box fall back to Homebrew.
if(APPLE)
  list(APPEND CMAKE_PREFIX_PATH "/opt/homebrew" "/usr/local")
endif()
find_package(PROJ CONFIG REQUIRED)

build_static_extension(${TARGET_NAME} ${EXTENSION_SOURCES})
build_loadable_extension(${TARGET_NAME} " " ${EXTENSION_SOURCES})

target_link_libraries(${EXTENSION_NAME} PROJ::proj)
target_link_libraries(${LOADABLE_EXTENSION_NAME} PROJ::proj)

install(
  TARGETS ${EXTENSION_NAME}
  EXPORT "${DUCKDB_EXPORT_SET}"
  LIBRARY DESTINATION "${INSTALL_LIB_DIR}"
  ARCHIVE DESTINATION "${INSTALL_LIB_DIR}")
