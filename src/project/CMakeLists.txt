# src/project/CMakeLists.txt
# Project module: .idhmfis project file serialisation/deserialisation
# (nlohmann/json), cue list management, undo/redo stack, asset registry,
# and auto-save / crash recovery.

# Collect only the top-level .cpp files (not the tests/ subdirectory).
file(GLOB _project_sources
    CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp"
)

list(APPEND IDHMFIS_SOURCES ${_project_sources})
set(IDHMFIS_SOURCES ${IDHMFIS_SOURCES} PARENT_SCOPE)

# Unit tests (only when the test suite is enabled)
if(IDHMFIS_BUILD_TESTS)
    add_subdirectory(tests)
endif()
