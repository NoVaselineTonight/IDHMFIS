# src/generators/CMakeLists.txt
# Generators module: procedural laser effect generators, timeline automation,
# oscillators, pattern engines, and real-time effect parameter modulation.

file(GLOB_RECURSE _generators_sources
    CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp"
)

list(APPEND IDHMFIS_SOURCES ${_generators_sources})
set(IDHMFIS_SOURCES ${IDHMFIS_SOURCES} PARENT_SCOPE)
