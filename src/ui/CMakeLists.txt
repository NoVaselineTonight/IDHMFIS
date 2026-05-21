# src/ui/CMakeLists.txt
# UI module: Dear ImGui-based interface, docking layout, timeline editor,
# sequence view, cue list, property panels, and Vulkan-backed window management.

file(GLOB_RECURSE _ui_sources
    CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp"
)

list(APPEND IDHMFIS_SOURCES ${_ui_sources})
set(IDHMFIS_SOURCES ${IDHMFIS_SOURCES} PARENT_SCOPE)
