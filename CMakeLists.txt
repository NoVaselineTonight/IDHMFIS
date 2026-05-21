cmake_minimum_required(VERSION 3.25)

project(IDHMFIS
    VERSION 1.5.0
    DESCRIPTION "IDHMFIS - Professional Laser Show Programming Application"
    LANGUAGES CXX
)

# ─────────────────────────────────────────────
# C++ standard
# ─────────────────────────────────────────────
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ─────────────────────────────────────────────
# CMake modules
# ─────────────────────────────────────────────
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

include(CompilerWarnings)
include(ClangTidy)
include(FindNDI)
include(FetchContent)
include(GNUInstallDirs)

# All FetchContent deps build static by default — prevents KissFFT conflict
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)

# ─────────────────────────────────────────────
# Build options
# ─────────────────────────────────────────────
option(ENABLE_CLANG_TIDY    "Enable clang-tidy static analysis"  OFF)
option(ENABLE_NDI           "Build with NDI SDK support"          OFF)
option(IDHMFIS_BUILD_TESTS  "Build benchmark / unit tests"        ON)
option(IDHMFIS_BUILD_TOOLS  "Build development tools"             ON)

# ─────────────────────────────────────────────
# Platform detection macros
# ─────────────────────────────────────────────
if(WIN32)
    add_compile_definitions(IDHMFIS_WINDOWS NOMINMAX _CRT_SECURE_NO_WARNINGS _WINSOCK_DEPRECATED_NO_WARNINGS)
elseif(APPLE)
    add_compile_definitions(IDHMFIS_MACOS)
elseif(UNIX)
    add_compile_definitions(IDHMFIS_LINUX)
endif()

# ─────────────────────────────────────────────
# Build-type defaults
# ─────────────────────────────────────────────
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE "Release" CACHE STRING "Build type" FORCE)
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS
        "Debug" "Release" "RelWithDebInfo" "MinSizeRel")
endif()

# LTO for Release builds
include(CheckIPOSupported)
check_ipo_supported(RESULT _ipo_supported OUTPUT _ipo_msg)
if(_ipo_supported)
    cmake_policy(SET CMP0069 NEW)
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELWITHDEBINFO OFF)
endif()

# ─────────────────────────────────────────────
# Per-configuration compiler flags
# ─────────────────────────────────────────────
if(MSVC)
    add_compile_options(
        "$<$<CONFIG:Debug>:/Od>"
        "$<$<CONFIG:Debug>:/Zi>"
        "$<$<CONFIG:Debug>:/RTC1>"
        "$<$<CONFIG:Release>:/O2>"
        "$<$<CONFIG:Release>:/GL>"
        "$<$<CONFIG:RelWithDebInfo>:/O2>"
        "$<$<CONFIG:RelWithDebInfo>:/Zi>"
    )
    add_link_options(
        "$<$<CONFIG:Release>:/LTCG>"
        "$<$<CONFIG:Debug>:/DEBUG:FULL>"
        "$<$<CONFIG:RelWithDebInfo>:/DEBUG:FULL>"
    )
else()
    add_compile_options(
        "$<$<CONFIG:Debug>:-O0>"
        "$<$<CONFIG:Debug>:-g3>"
        "$<$<CONFIG:Release>:-O3>"
        "$<$<CONFIG:RelWithDebInfo>:-O2>"
        "$<$<CONFIG:RelWithDebInfo>:-g>"
    )
endif()

# ─────────────────────────────────────────────
# Optional clang-tidy
# ─────────────────────────────────────────────
if(ENABLE_CLANG_TIDY)
    enable_clang_tidy()
endif()

# ─────────────────────────────────────────────
# FetchContent declarations
# ─────────────────────────────────────────────

# SDL3
FetchContent_Declare(
    SDL3
    GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
    GIT_TAG        main
    GIT_SHALLOW    TRUE
)

# Dear ImGui — docking branch
FetchContent_Declare(
    imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG        docking
    GIT_SHALLOW    TRUE
)

# nlohmann/json 3.11.x
FetchContent_Declare(
    nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG        v3.11.3
    GIT_SHALLOW    TRUE
)

# RtMidi
FetchContent_Declare(
    rtmidi
    GIT_REPOSITORY https://github.com/thestk/rtmidi.git
    GIT_TAG        master
    GIT_SHALLOW    TRUE
)

# readerwriterqueue (lock-free SPSC)
FetchContent_Declare(
    readerwriterqueue
    GIT_REPOSITORY https://github.com/cameron314/readerwriterqueue.git
    GIT_TAG        master
    GIT_SHALLOW    TRUE
)

# concurrentqueue (MPSC/MPMC)
FetchContent_Declare(
    concurrentqueue
    GIT_REPOSITORY https://github.com/cameron314/concurrentqueue.git
    GIT_TAG        master
    GIT_SHALLOW    TRUE
)

# KissFFT
FetchContent_Declare(
    kissfft
    GIT_REPOSITORY https://github.com/mborgerding/kissfft.git
    GIT_TAG        master
    GIT_SHALLOW    TRUE
)

# Vulkan Memory Allocator (used on non-Windows)
FetchContent_Declare(
    VulkanMemoryAllocator
    GIT_REPOSITORY https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator.git
    GIT_TAG        master
    GIT_SHALLOW    TRUE
)

# hidapi — Windows HID library needed by LaserDock DAC driver.
# Built as a shared DLL so LaserDockDac can LoadLibrary("hidapi.dll") at runtime.
if(WIN32)
    FetchContent_Declare(
        hidapi
        GIT_REPOSITORY https://github.com/libusb/hidapi.git
        GIT_TAG        hidapi-0.14.0
        GIT_SHALLOW    TRUE
    )
endif()

# ─────────────────────────────────────────────
# FetchContent population
# ─────────────────────────────────────────────

# SDL3 — disable extras
set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON  CACHE BOOL "" FORCE)
set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(SDL3)

# nlohmann_json — header-only, cmake support
FetchContent_MakeAvailable(nlohmann_json)

# RtMidi — turn off examples/tests
set(RTMIDI_BUILD_TESTING OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(rtmidi)

# readerwriterqueue — header-only, no CMake needed
FetchContent_MakeAvailable(readerwriterqueue)

# concurrentqueue — header-only
FetchContent_MakeAvailable(concurrentqueue)

# KissFFT — static, no tools, no tests
# BUILD_SHARED_LIBS=OFF (set globally above) prevents the KISSFFT_STATIC conflict
set(KISSFFT_TEST       OFF CACHE BOOL "" FORCE)
set(KISSFFT_TOOLS      OFF CACHE BOOL "" FORCE)
set(KISSFFT_STATIC     ON  CACHE BOOL "" FORCE)
set(KISSFFT_USE_ALLOCA OFF CACHE BOOL "" FORCE)
# Suppress doctest's old cmake_minimum_required (pulled in by kissfft unconditionally)
set(CMAKE_POLICY_VERSION_MINIMUM 3.5 CACHE STRING "" FORCE)
FetchContent_MakeAvailable(kissfft)

# VulkanMemoryAllocator — header-only interface
set(VMA_BUILD_DOCUMENTATION OFF CACHE BOOL "" FORCE)
set(VMA_BUILD_SAMPLES        OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(VulkanMemoryAllocator)

# hidapi — shared DLL for LaserDock runtime loading (Windows only)
if(WIN32)
    set(BUILD_SHARED_LIBS ON CACHE BOOL "" FORCE)
    set(HIDAPI_BUILD_HIDTEST OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(hidapi)
    set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
endif()

# ─────────────────────────────────────────────
# Vulkan — only required on non-Windows
# ─────────────────────────────────────────────
if(NOT WIN32)
    find_package(Vulkan REQUIRED)
else()
    # Windows uses D3D12; Vulkan is optional
    find_package(Vulkan QUIET)
endif()

# ─────────────────────────────────────────────
# VMA interface target (Vulkan path only)
# ─────────────────────────────────────────────
add_library(vma_interface INTERFACE)
target_include_directories(vma_interface INTERFACE
    ${vulkanmemoryallocator_SOURCE_DIR}/include
)
if(Vulkan_FOUND)
    target_link_libraries(vma_interface INTERFACE Vulkan::Vulkan)
endif()
add_library(VulkanMemoryAllocator::VulkanMemoryAllocator ALIAS vma_interface)

# ─────────────────────────────────────────────
# readerwriterqueue interface target
# ─────────────────────────────────────────────
add_library(readerwriterqueue_iface INTERFACE)
target_include_directories(readerwriterqueue_iface INTERFACE
    ${readerwriterqueue_SOURCE_DIR}
)
add_library(readerwriterqueue::readerwriterqueue ALIAS readerwriterqueue_iface)

# ─────────────────────────────────────────────
# concurrentqueue interface target
# ─────────────────────────────────────────────
add_library(concurrentqueue_iface INTERFACE)
target_include_directories(concurrentqueue_iface INTERFACE
    ${concurrentqueue_SOURCE_DIR}
)
add_library(concurrentqueue::concurrentqueue ALIAS concurrentqueue_iface)

# ─────────────────────────────────────────────
# Dear ImGui — manual target
# Platform-specific backend selection:
#   Windows  → SDL3 + DX12
#   macOS    → SDL3 + Vulkan
# ─────────────────────────────────────────────
FetchContent_MakeAvailable(imgui)

set(_imgui_core_sources
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_demo.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_sdl3.cpp
)

if(WIN32)
    list(APPEND _imgui_core_sources
        ${imgui_SOURCE_DIR}/backends/imgui_impl_dx12.cpp
    )
else()
    list(APPEND _imgui_core_sources
        ${imgui_SOURCE_DIR}/backends/imgui_impl_vulkan.cpp
    )
endif()

add_library(imgui STATIC ${_imgui_core_sources})

target_include_directories(imgui
    PUBLIC
        ${imgui_SOURCE_DIR}
        ${imgui_SOURCE_DIR}/backends
)

# ImGui SDL3 dependency
target_link_libraries(imgui PUBLIC SDL3::SDL3-static)

if(WIN32)
    # D3D12 headers are in the Windows SDK — no extra include needed
    target_link_libraries(imgui PUBLIC d3d12 dxgi d3dcompiler dwmapi)
elseif(Vulkan_FOUND)
    target_link_libraries(imgui PUBLIC Vulkan::Vulkan)
endif()

# ─────────────────────────────────────────────
# Optional NDI
# ─────────────────────────────────────────────
if(ENABLE_NDI)
    find_package(NDI QUIET)
    if(NDI_FOUND)
        add_compile_definitions(HAVE_NDI)
        message(STATUS "NDI SDK found: ${NDI_INCLUDE_DIR}")
    else()
        message(WARNING "ENABLE_NDI=ON but NDI SDK not found; NDI support disabled")
        set(ENABLE_NDI OFF)
    endif()
endif()

# ─────────────────────────────────────────────
# Subdirectories
# ─────────────────────────────────────────────
add_subdirectory(src)

if(IDHMFIS_BUILD_TOOLS)
    add_subdirectory(tools/artnet_test_sender)
endif()

if(IDHMFIS_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests/bench)
endif()

# ─────────────────────────────────────────────
# Install rules
# ─────────────────────────────────────────────
install(TARGETS IDHMFIS
    RUNTIME DESTINATION dist
)

install(DIRECTORY "${CMAKE_SOURCE_DIR}/assets/"
    DESTINATION "dist/assets"
    OPTIONAL
)
