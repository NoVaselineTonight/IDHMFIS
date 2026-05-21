# src/core/CMakeLists.txt
# Core module: event loop, application lifecycle, logging, config management,
# utility types (math, colour, geometry), and cross-platform OS abstractions.

file(GLOB_RECURSE _core_sources
    CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp"
)

# Append to the parent's source list
list(APPEND IDHMFIS_SOURCES ${_core_sources})
set(IDHMFIS_SOURCES ${IDHMFIS_SOURCES} PARENT_SCOPE)
