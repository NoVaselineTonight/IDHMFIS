# cmake/FindNDI.cmake
# Locates the NewTek / Vizrt NDI SDK (version 6 preferred, fallback to v5).
#
# Imported target created on success:
#   NDI::NDI
#
# Variables set:
#   NDI_FOUND          — TRUE when SDK is located
#   NDI_INCLUDE_DIR    — path to Processing.NDI.Lib.x64.h (or .x86.h)
#   NDI_LIBRARY        — full path to the import library (.lib on Windows,
#                        .dylib stub on macOS)
#   NDI_VERSION        — detected major version (6 or 5)
#
# Search order on Windows:
#   1. NDI_SDK_DIR cmake variable / environment variable
#   2. Registry: HKLM\SOFTWARE\NDI\NDI 6 SDK   (install location)
#   3. Registry: HKLM\SOFTWARE\NDI\NDI 5 SDK
#   4. %ProgramFiles%\NDI\NDI 6 SDK
#   5. %ProgramFiles%\NDI\NDI 5 SDK
#
# Search order on macOS:
#   1. NDI_SDK_DIR cmake variable / environment variable
#   2. /Library/NDI SDK for Apple/
#   3. ~/Library/NDI SDK for Apple/
#
# Usage in project:
#   find_package(NDI)          # optional
#   if(NDI_FOUND)
#       target_link_libraries(myTarget PRIVATE NDI::NDI)
#   endif()

# ──────────────────────────────────────────────────────────────────
# Collect candidate root paths
# ──────────────────────────────────────────────────────────────────
set(_ndi_search_roots "")

# User-supplied hint (cmake var or env var)
if(NDI_SDK_DIR)
    list(APPEND _ndi_search_roots "${NDI_SDK_DIR}")
endif()
if(DEFINED ENV{NDI_SDK_DIR})
    list(APPEND _ndi_search_roots "$ENV{NDI_SDK_DIR}")
endif()

if(WIN32)
    # Registry queries — CMake 3.24+ cmake_host_system_information or
    # get_filename_component with REGISTRY are available on all 3.25 targets.

    foreach(_ver 6 5)
        set(_reg_key
            "HKLM/SOFTWARE/NDI/NDI ${_ver} SDK;InstallationDirectory")
        cmake_host_system_information(
            RESULT _reg_path
            QUERY WINDOWS_REGISTRY
                "HKLM/SOFTWARE/NDI/NDI ${_ver} SDK"
                VALUE "InstallationDirectory"
                VIEW BOTH
            ERROR_VARIABLE _reg_err
        )
        if(_reg_path AND NOT _reg_err)
            list(APPEND _ndi_search_roots "${_reg_path}")
        endif()

        # Also try 32-bit view node explicitly
        cmake_host_system_information(
            RESULT _reg_path32
            QUERY WINDOWS_REGISTRY
                "HKLM/SOFTWARE/WOW6432Node/NDI/NDI ${_ver} SDK"
                VALUE "InstallationDirectory"
                VIEW 32
            ERROR_VARIABLE _reg_err32
        )
        if(_reg_path32 AND NOT _reg_err32)
            list(APPEND _ndi_search_roots "${_reg_path32}")
        endif()
    endforeach()

    # Hard-coded default install locations
    foreach(_ver 6 5)
        list(APPEND _ndi_search_roots
            "$ENV{ProgramFiles}/NDI/NDI ${_ver} SDK"
            "$ENV{ProgramFiles}/NewTek/NDI ${_ver} SDK"
            "C:/Program Files/NDI/NDI ${_ver} SDK"
            "C:/Program Files/NewTek/NDI ${_ver} SDK"
        )
    endforeach()

elseif(APPLE)
    list(APPEND _ndi_search_roots
        "/Library/NDI SDK for Apple"
        "$ENV{HOME}/Library/NDI SDK for Apple"
        "/opt/ndi-sdk"
    )
endif()

# ──────────────────────────────────────────────────────────────────
# Find the header
# ──────────────────────────────────────────────────────────────────
find_path(NDI_INCLUDE_DIR
    NAMES
        Processing.NDI.Lib.x64.h
        Processing.NDI.Lib.x86.h
        Processing.NDI.Lib.h
    HINTS
        ${_ndi_search_roots}
    PATH_SUFFIXES
        include
        Include
    NO_DEFAULT_PATH
)

# ──────────────────────────────────────────────────────────────────
# Find the library
# ──────────────────────────────────────────────────────────────────
if(WIN32)
    find_library(NDI_LIBRARY
        NAMES
            Processing.NDI.Lib.x64
            Processing.NDI.Lib.x86
            ndi
        HINTS
            ${_ndi_search_roots}
        PATH_SUFFIXES
            Lib/x64
            Lib/x86
            lib/x64
            lib
        NO_DEFAULT_PATH
    )
elseif(APPLE)
    find_library(NDI_LIBRARY
        NAMES
            ndi
            libndi
        HINTS
            ${_ndi_search_roots}
        PATH_SUFFIXES
            lib
            Lib
        NO_DEFAULT_PATH
    )
endif()

# ──────────────────────────────────────────────────────────────────
# Detect version from the path string
# ──────────────────────────────────────────────────────────────────
set(NDI_VERSION "")
if(NDI_INCLUDE_DIR)
    if("${NDI_INCLUDE_DIR}" MATCHES "NDI 6")
        set(NDI_VERSION 6)
    elseif("${NDI_INCLUDE_DIR}" MATCHES "NDI 5")
        set(NDI_VERSION 5)
    else()
        set(NDI_VERSION 6) # assume latest if unknown
    endif()
endif()

# ──────────────────────────────────────────────────────────────────
# Standard find_package result handling
# ──────────────────────────────────────────────────────────────────
include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(NDI
    REQUIRED_VARS NDI_INCLUDE_DIR NDI_LIBRARY
    VERSION_VAR   NDI_VERSION
)

# ──────────────────────────────────────────────────────────────────
# Create imported target
# ──────────────────────────────────────────────────────────────────
if(NDI_FOUND AND NOT TARGET NDI::NDI)
    add_library(NDI::NDI UNKNOWN IMPORTED)
    set_target_properties(NDI::NDI PROPERTIES
        IMPORTED_LOCATION             "${NDI_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${NDI_INCLUDE_DIR}"
    )

    # On Windows the DLL must be next to the executable at runtime.
    # Propagate the DLL path so install rules can copy it.
    if(WIN32)
        find_file(NDI_RUNTIME_DLL
            NAMES
                Processing.NDI.Lib.x64.dll
                ndi.dll
            HINTS
                ${_ndi_search_roots}
            PATH_SUFFIXES
                Bin/x64
                bin/x64
                bin
            NO_DEFAULT_PATH
        )
        if(NDI_RUNTIME_DLL)
            set_target_properties(NDI::NDI PROPERTIES
                IMPORTED_RUNTIME "${NDI_RUNTIME_DLL}"
            )
            message(STATUS "NDI runtime DLL: ${NDI_RUNTIME_DLL}")
        endif()
    endif()
endif()

mark_as_advanced(NDI_INCLUDE_DIR NDI_LIBRARY NDI_RUNTIME_DLL)
