# src/capture/CMakeLists.txt
#
# Two targets on Windows:
#   1. Sources added to IDHMFIS (EXE) — virtual_camera.cpp (SHM producer)
#   2. IDHMFISCapture.dll              — idhmfis_camera.cpp (COM server / SHM consumer)
#
# On non-Windows everything is a no-op stub already handled by #ifdef guards.

if(WIN32)
    # ── EXE sources: everything except idhmfis_camera.cpp ──────────────────────
    file(GLOB _capture_exe_sources
        CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp"
    )
    list(REMOVE_ITEM _capture_exe_sources
        "${CMAKE_CURRENT_SOURCE_DIR}/idhmfis_camera.cpp"
    )
    list(APPEND IDHMFIS_SOURCES ${_capture_exe_sources})
    set(IDHMFIS_SOURCES ${IDHMFIS_SOURCES} PARENT_SCOPE)

    # ── IDHMFISCapture.dll ────────────────────────────────────────────────────
    add_library(IDHMFISCapture SHARED
        "${CMAKE_CURRENT_SOURCE_DIR}/idhmfis_camera.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/IDHMFISCapture.def"
    )

    target_include_directories(IDHMFISCapture
        PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}"         # camera_shm.h
            "${CMAKE_CURRENT_SOURCE_DIR}/../"     # src/ root (for core/types.h etc.)
    )

    target_link_libraries(IDHMFISCapture
        PRIVATE
            ole32
            oleaut32
            mfplat
            mfuuid
            uuid
    )

    # Suppress the same warnings as the main EXE.
    idhmfis_set_warnings(IDHMFISCapture)

    # Land the DLL alongside IDHMFIS.exe so InprocServer32 resolves correctly.
    set_target_properties(IDHMFISCapture PROPERTIES
        OUTPUT_NAME  "IDHMFISCapture"
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    )

else()
    # Non-Windows: only virtual_camera.cpp (which is all stubs).
    file(GLOB _capture_exe_sources
        CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp"
    )
    list(APPEND IDHMFIS_SOURCES ${_capture_exe_sources})
    set(IDHMFIS_SOURCES ${IDHMFIS_SOURCES} PARENT_SCOPE)
endif()
