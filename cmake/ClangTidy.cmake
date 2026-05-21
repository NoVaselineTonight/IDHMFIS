# cmake/ClangTidy.cmake
# Optional clang-tidy integration.
# Call enable_clang_tidy() after including this module to activate it.
# Requires ENABLE_CLANG_TIDY=ON (set from root CMakeLists.txt).

function(enable_clang_tidy)
    find_program(CLANG_TIDY_EXE
        NAMES clang-tidy-18 clang-tidy-17 clang-tidy-16 clang-tidy
        DOC "Path to clang-tidy executable"
    )

    if(NOT CLANG_TIDY_EXE)
        message(WARNING
            "clang-tidy not found. "
            "Install clang-tidy or set CLANG_TIDY_EXE to its path. "
            "Static analysis will be skipped."
        )
        return()
    endif()

    message(STATUS "clang-tidy enabled: ${CLANG_TIDY_EXE}")

    # Pass the .clang-tidy config file explicitly so it is picked up
    # regardless of the working directory CMake invokes the tool from.
    set(_config_arg "")
    if(EXISTS "${CMAKE_SOURCE_DIR}/.clang-tidy")
        # clang-tidy ≥ 12 supports --config-file=
        execute_process(
            COMMAND ${CLANG_TIDY_EXE} --version
            OUTPUT_VARIABLE _ct_version_str
            ERROR_QUIET
            OUTPUT_STRIP_TRAILING_WHITESPACE
        )
        string(REGEX MATCH "([0-9]+)\\.([0-9]+)" _ct_ver "${_ct_version_str}")
        if(CMAKE_MATCH_1 GREATER_EQUAL 12)
            set(_config_arg "--config-file=${CMAKE_SOURCE_DIR}/.clang-tidy")
        endif()
    endif()

    # Enable compile_commands.json so clang-tidy can resolve includes
    set(CMAKE_EXPORT_COMPILE_COMMANDS ON PARENT_SCOPE)

    set(CMAKE_CXX_CLANG_TIDY
        "${CLANG_TIDY_EXE}"
        ${_config_arg}
        "--header-filter=.*"
        "--extra-arg=-std=c++20"
        PARENT_SCOPE
    )
endfunction()

# ──────────────────────────────────────────────────────────────────
# Helper: disable clang-tidy on a single target (e.g., third-party)
# Usage: idhmfis_disable_clang_tidy(<target>)
# ──────────────────────────────────────────────────────────────────
function(idhmfis_disable_clang_tidy target)
    set_target_properties(${target} PROPERTIES
        CXX_CLANG_TIDY ""
    )
endfunction()
