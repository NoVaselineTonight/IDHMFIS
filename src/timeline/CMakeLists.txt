# src/timeline/CMakeLists.txt
# idhmfis_timeline — SMPTE timecode-driven timeline engine (Part A)
# Sources appended to IDHMFIS_SOURCES via PARENT_SCOPE; no separate static lib.

set(_timeline_sources
    "${CMAKE_CURRENT_SOURCE_DIR}/timeline_engine.cpp"
)

list(APPEND IDHMFIS_SOURCES ${_timeline_sources})
set(IDHMFIS_SOURCES ${IDHMFIS_SOURCES} PARENT_SCOPE)
