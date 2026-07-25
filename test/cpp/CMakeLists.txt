cmake_minimum_required(VERSION 3.5)

# Standalone C++ unit test executable for the three_d kernel
project(three_d_unit_tests)

# Root of the repository
get_filename_component(REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)

# Catch2 from DuckDB third_party
set(CATCH_INCLUDE_DIR "${REPO_ROOT}/duckdb/third_party/catch")

# Kernel sources (compiled directly into test binary)
set(KERNEL_SOURCES
    ${REPO_ROOT}/src/kernel/solid_model.cpp
    ${REPO_ROOT}/src/kernel/payload.cpp
    ${REPO_ROOT}/src/kernel/wkb_parser.cpp
    ${REPO_ROOT}/src/kernel/model_builder.cpp
    ${REPO_ROOT}/src/kernel/wkb_export.cpp
    ${REPO_ROOT}/src/kernel/validation.cpp
    ${REPO_ROOT}/src/kernel/triangulation.cpp
    ${REPO_ROOT}/src/kernel/measurements.cpp
    ${REPO_ROOT}/src/kernel/metadata_parser.cpp
    ${REPO_ROOT}/src/kernel/geom_model.cpp
    ${REPO_ROOT}/src/kernel/geom_wkb_parser.cpp
    ${REPO_ROOT}/src/kernel/geom_payload.cpp
    ${REPO_ROOT}/src/kernel/geom_distance.cpp
    ${REPO_ROOT}/src/kernel/geom_construct.cpp
    ${REPO_ROOT}/src/kernel/geom_analysis.cpp
    ${REPO_ROOT}/src/kernel/geom_serialize.cpp
    ${REPO_ROOT}/src/kernel/crs_transform.cpp
    ${REPO_ROOT}/src/kernel/arrow_native_import.cpp)

# Test sources
set(TEST_SOURCES
    test_main.cpp
    test_payload.cpp
    test_wkb_parser.cpp
    test_model_builder.cpp
    test_validation.cpp
    test_measurements.cpp
    test_metadata.cpp
    test_geom_wkb_parser.cpp
    test_geom_distance.cpp
    test_geom_construct.cpp
    test_geom_analysis.cpp
    test_geom_serialize.cpp
    test_crs_transform.cpp
    test_wkb_export.cpp
    test_inner_shell.cpp
    test_triangulation.cpp
    test_arrow_native_import.cpp)

add_executable(three_d_unit_tests ${TEST_SOURCES} ${KERNEL_SOURCES})

target_include_directories(three_d_unit_tests PRIVATE ${CATCH_INCLUDE_DIR}
                                                      ${REPO_ROOT}/src/include)

# PROJ (crs_transform.cpp). Under the vcpkg toolchain its copy is found first;
# on a bare macOS/Linux dev box fall back to the Homebrew/system install.
if(APPLE)
  list(APPEND CMAKE_PREFIX_PATH "/opt/homebrew" "/usr/local")
endif()
find_package(PROJ CONFIG REQUIRED)
target_link_libraries(three_d_unit_tests PRIVATE PROJ::proj)
