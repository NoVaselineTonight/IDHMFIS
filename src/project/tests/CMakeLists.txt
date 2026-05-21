# src/project/tests/CMakeLists.txt
# Unit tests for the project module.
# Built when IDHMFIS_BUILD_TESTS=ON (the default).

cmake_minimum_required(VERSION 3.25)

# ── Fetch doctest ─────────────────────────────────────────────────────────────
include(FetchContent)

FetchContent_Declare(
    doctest
    GIT_REPOSITORY https://github.com/doctest/doctest.git
    GIT_TAG        v2.4.11
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(doctest)

# Expose doctest_discover_tests() cmake module
list(APPEND CMAKE_MODULE_PATH "${doctest_SOURCE_DIR}/scripts/cmake")

# ── Test executable ───────────────────────────────────────────────────────────
add_executable(test_project
    test_project.cpp

    # Project module sources needed for linking
    ${CMAKE_CURRENT_SOURCE_DIR}/../cue.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../project.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../serialization.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../default_fixture.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../undo_redo.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../project_io.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../autosave.cpp
)

target_include_directories(test_project
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/..
        ${CMAKE_CURRENT_SOURCE_DIR}/../..
        ${doctest_SOURCE_DIR}
)

target_link_libraries(test_project
    PRIVATE
        nlohmann_json::nlohmann_json
        doctest::doctest
)

# Platform-specific libs (needed by project_io.cpp / autosave.cpp on Windows)
if(WIN32)
    target_link_libraries(test_project PRIVATE shell32)
endif()

target_compile_features(test_project PRIVATE cxx_std_20)

# ── Register with CTest ───────────────────────────────────────────────────────
include(doctest)
doctest_discover_tests(test_project)

# Also register as a plain ctest entry in case doctest cmake integration
# is unavailable
add_test(NAME project_tests COMMAND test_project)
