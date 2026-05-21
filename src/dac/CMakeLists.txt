# src/dac/CMakeLists.txt
# DAC module: laser DAC output drivers.
# Supports EtherDream (TCP), Helios (USB), ILDA (file/playback),
# and generic ILDA-over-network protocols.
# Uses lock-free SPSC queues (readerwriterqueue) for the hot output path.

file(GLOB_RECURSE _dac_sources
    CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp"
)

list(APPEND IDHMFIS_SOURCES ${_dac_sources})
set(IDHMFIS_SOURCES ${IDHMFIS_SOURCES} PARENT_SCOPE)
