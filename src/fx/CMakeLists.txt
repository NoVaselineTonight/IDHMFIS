# src/fx/CMakeLists.txt
# FX engine module: signal-chain processing blocks, modulation matrix,
# expression VM, and strobe subsystem.

file(GLOB_RECURSE _fx_sources
    CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/blocks/*.cpp"
)

# Append to the parent's source list
list(APPEND IDHMFIS_SOURCES ${_fx_sources})
set(IDHMFIS_SOURCES ${IDHMFIS_SOURCES} PARENT_SCOPE)
