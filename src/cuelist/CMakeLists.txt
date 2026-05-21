# src/cuelist/CMakeLists.txt
# idhmfis_cuelist — standalone MagicQ-class cue list library (spec §B7 + §B8)
# Sources appended to IDHMFIS_SOURCES via PARENT_SCOPE; no separate static lib.

set(_cuelist_sources
    "${CMAKE_CURRENT_SOURCE_DIR}/cuelist.cpp"
)

list(APPEND IDHMFIS_SOURCES ${_cuelist_sources})
set(IDHMFIS_SOURCES ${IDHMFIS_SOURCES} PARENT_SCOPE)
