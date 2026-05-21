# src/input/CMakeLists.txt
# Input module: MIDI input (via RtMidi), OSC receiver, DMX (ArtNet/sACN)
# input handling, keyboard/mouse event mapping, and controller abstraction.

file(GLOB_RECURSE _input_sources
    CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp"
)

list(APPEND IDHMFIS_SOURCES ${_input_sources})
set(IDHMFIS_SOURCES ${IDHMFIS_SOURCES} PARENT_SCOPE)
