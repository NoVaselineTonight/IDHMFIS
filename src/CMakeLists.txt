# src/CMakeLists.txt
# Collects all module sources, defines the main executable target,
# and wires up all link dependencies.

# ──────────────────────────────────────────────────────────────────
# Collect source files from each module subdirectory.
# Each sub-CMakeLists.txt appends to IDHMFIS_SOURCES via
# the PARENT_SCOPE trick so we can maintain a single target.
# ──────────────────────────────────────────────────────────────────
set(IDHMFIS_SOURCES "")

add_subdirectory(core)
add_subdirectory(fx)
add_subdirectory(render)
add_subdirectory(generators)
add_subdirectory(input)
add_subdirectory(dac)
add_subdirectory(capture)
add_subdirectory(ui)
add_subdirectory(audio)
add_subdirectory(project)
add_subdirectory(cuelist)
add_subdirectory(zones)
add_subdirectory(safety)
add_subdirectory(timeline)

# ──────────────────────────────────────────────────────────────────
# Main executable
# ──────────────────────────────────────────────────────────────────
if(WIN32)
    # WIN32 keyword = WinMain entry point / no console window in Release.
    # We re-attach a console in Debug via AllocConsole() in main.cpp.
    add_executable(IDHMFIS WIN32
        "${CMAKE_CURRENT_SOURCE_DIR}/main.cpp"
        ${IDHMFIS_SOURCES}
    )
else()
    add_executable(IDHMFIS
        "${CMAKE_CURRENT_SOURCE_DIR}/main.cpp"
        ${IDHMFIS_SOURCES}
    )
endif()

# ──────────────────────────────────────────────────────────────────
# Include paths
# ──────────────────────────────────────────────────────────────────
target_include_directories(IDHMFIS
    PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${CMAKE_CURRENT_SOURCE_DIR}/core"
        "${CMAKE_CURRENT_SOURCE_DIR}/fx"
        "${CMAKE_CURRENT_SOURCE_DIR}/render"
        "${CMAKE_CURRENT_SOURCE_DIR}/generators"
        "${CMAKE_CURRENT_SOURCE_DIR}/input"
        "${CMAKE_CURRENT_SOURCE_DIR}/dac"
        "${CMAKE_CURRENT_SOURCE_DIR}/capture"
        "${CMAKE_CURRENT_SOURCE_DIR}/ui"
        "${CMAKE_CURRENT_SOURCE_DIR}/audio"
        "${CMAKE_CURRENT_SOURCE_DIR}/project"
        "${CMAKE_CURRENT_SOURCE_DIR}/cuelist"
        "${CMAKE_CURRENT_SOURCE_DIR}/zones"
        "${CMAKE_CURRENT_SOURCE_DIR}/safety"
        "${CMAKE_CURRENT_SOURCE_DIR}/timeline"
)

# ──────────────────────────────────────────────────────────────────
# Link libraries — always required
# ──────────────────────────────────────────────────────────────────
target_link_libraries(IDHMFIS
    PRIVATE
        imgui
        nlohmann_json::nlohmann_json
        rtmidi
        readerwriterqueue::readerwriterqueue
        concurrentqueue::concurrentqueue
        kissfft
        SDL3::SDL3-static
        VulkanMemoryAllocator::VulkanMemoryAllocator
)

# ──────────────────────────────────────────────────────────────────
# Windows-specific system libraries
# ──────────────────────────────────────────────────────────────────
if(WIN32)
    target_link_libraries(IDHMFIS
        PRIVATE
            ws2_32      # Winsock (ArtNet / OSC networking)
            winmm       # Windows multimedia (MIDI timing, wave audio)
            dwmapi      # Desktop Window Manager
            d3d12       # Direct3D 12 renderer
            dxgi        # DXGI swap chain / factory
            d3dcompiler # Shader compiler (runtime fallback)
            ole32       # WASAPI / COM initialisation
            oleaut32    # COM automation (CoTaskMemAlloc/Free, etc.)
            strmiids    # DirectShow media type / interface GUIDs
            uuid        # COM CLSID_FilterGraph, IID_* constants
            iphlpapi    # IP helper API (ArtNet interface enumeration)
            mfplat      # Media Foundation platform (MFStartup for virtual camera)
            mfuuid      # Media Foundation GUIDs (IID_IMFMediaSource etc.)
    )
else()
    # macOS / Linux: Vulkan renderer
    if(Vulkan_FOUND)
        target_link_libraries(IDHMFIS PRIVATE Vulkan::Vulkan)
    endif()
endif()

# ──────────────────────────────────────────────────────────────────
# Optional NDI
# ──────────────────────────────────────────────────────────────────
if(ENABLE_NDI AND NDI_FOUND)
    target_link_libraries(IDHMFIS PRIVATE NDI::NDI)
endif()

# ──────────────────────────────────────────────────────────────────
# Compiler warnings
# ──────────────────────────────────────────────────────────────────
idhmfis_set_warnings(IDHMFIS)

# ──────────────────────────────────────────────────────────────────
# Output name
# ──────────────────────────────────────────────────────────────────
set_target_properties(IDHMFIS PROPERTIES
    OUTPUT_NAME "IDHMFIS"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

# ──────────────────────────────────────────────────────────────────
# Copy runtime DLLs to exe directory on build
# ──────────────────────────────────────────────────────────────────
if(WIN32)
    # hidapi.dll — required by LaserDock driver (LoadLibrary at runtime)
    foreach(_hidapi_target hidapi hidapi::hidapi hidapi_winapi)
        if(TARGET ${_hidapi_target})
            add_custom_command(TARGET IDHMFIS POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    $<TARGET_FILE:${_hidapi_target}>
                    $<TARGET_FILE_DIR:IDHMFIS>
                COMMENT "Copying hidapi DLL to bin"
            )
            break()
        endif()
    endforeach()
endif()

# ──────────────────────────────────────────────────────────────────
# Disable clang-tidy on third-party targets pulled in transitively
# ──────────────────────────────────────────────────────────────────
if(ENABLE_CLANG_TIDY)
    foreach(_tp imgui rtmidi kissfft)
        if(TARGET ${_tp})
            idhmfis_disable_clang_tidy(${_tp})
        endif()
    endforeach()
endif()
