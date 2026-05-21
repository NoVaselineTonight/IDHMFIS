# src/audio/CMakeLists.txt
# Audio module: audio capture (WASAPI on Windows, CoreAudio on macOS),
# beat detection, FFT spectrum analysis (KissFFT), and audio-reactive
# parameter modulation pipelines.

file(GLOB_RECURSE _audio_sources
    CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp"
)

list(APPEND IDHMFIS_SOURCES ${_audio_sources})
set(IDHMFIS_SOURCES ${IDHMFIS_SOURCES} PARENT_SCOPE)
