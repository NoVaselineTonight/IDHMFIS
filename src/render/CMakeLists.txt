# src/render/CMakeLists.txt
# Render module: Vulkan renderer, swap-chain management, render passes,
# galvo/laser frame rasterisation, point-buffer upload, and shader loading.

file(GLOB_RECURSE _render_sources
    CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp"
)

list(APPEND IDHMFIS_SOURCES ${_render_sources})
set(IDHMFIS_SOURCES ${IDHMFIS_SOURCES} PARENT_SCOPE)
