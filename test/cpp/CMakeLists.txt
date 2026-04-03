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
)

# Test sources
set(TEST_SOURCES
    test_main.cpp
    test_payload.cpp
    test_wkb_parser.cpp
)

add_executable(three_d_unit_tests ${TEST_SOURCES} ${KERNEL_SOURCES})

target_include_directories(three_d_unit_tests PRIVATE
    ${CATCH_INCLUDE_DIR}
    ${REPO_ROOT}/src/include
)
