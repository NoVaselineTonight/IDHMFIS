# tools/artnet_test_sender/CMakeLists.txt
# Standalone ArtNet test-sender tool for verifying DAC network output.
# Sends synthetic ArtNet DMX512 frames to a configurable IP:port.

file(GLOB_RECURSE _artnet_sources
    CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp"
)

if(NOT _artnet_sources)
    # Stub main so the target compiles even before implementation is written
    set(_stub "${CMAKE_CURRENT_BINARY_DIR}/artnet_stub.cpp")
    file(WRITE "${_stub}" "// Auto-generated stub\nint main() { return 0; }\n")
    list(APPEND _artnet_sources "${_stub}")
endif()

add_executable(artnet_test_sender ${_artnet_sources})

target_include_directories(artnet_test_sender
    PRIVATE
        "${CMAKE_SOURCE_DIR}/src"
)

target_link_libraries(artnet_test_sender
    PRIVATE
        nlohmann_json::nlohmann_json
)

if(WIN32)
    target_link_libraries(artnet_test_sender PRIVATE ws2_32)
endif()

idhmfis_set_warnings(artnet_test_sender)

set_target_properties(artnet_test_sender PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)
