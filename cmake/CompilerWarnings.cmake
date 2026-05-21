# cmake/CompilerWarnings.cmake
# Sets strict per-compiler warning flags on a target (or globally when called
# without a target).  Call idhmfis_set_warnings(<target>) for per-target use,
# or call idhmfis_set_global_warnings() to apply flags to every subsequent
# target in the directory scope.

# ──────────────────────────────────────────────────────────────────
# Internal helper — returns the flag list in the named variable
# ──────────────────────────────────────────────────────────────────
function(_idhmfis_warning_flags outvar)
    if(MSVC)
        set(${outvar}
            /W4          # High warning level (excludes /Wall which is too noisy)
            /WX          # Treat warnings as errors
            /permissive- # Strict conformance
            /w14242      # Possible loss of data (narrowing)
            /w14254      # Larger bit-field type to smaller
            /w14263      # Member function does not override base class virtual
            /w14265      # Class has virtual functions but destructor not virtual
            /w14287      # Unsigned/negative constant mismatch
            /we4289      # Non-standard extension: loop control variable
            /w14296      # Expression is always false/true
            /w14311      # Pointer truncation
            /w14545      # Expression before comma evaluates to a function
            /w14546      # Function call before comma missing argument list
            /w14547      # Operator before comma has no effect
            /w14549      # Operator before comma has no effect
            /w14555      # Expression has no effect
            /w14619      # Pragma warning: non-existent warning number
            /w14640      # Thread unsafe static member initialisation
            /w14826      # Conversion from type1 to type2 is sign-extended
            /w14905      # Wide string literal cast to LPSTR
            /w14906      # String literal cast to LPWSTR
            /w14928      # Illegal copy-initialisation
            PARENT_SCOPE
        )
    elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        set(${outvar}
            -Wall
            -Wextra
            -Werror
            -Wpedantic
            -Wshadow
            -Wnon-virtual-dtor
            -Wold-style-cast
            -Wcast-align
            -Wunused
            -Woverloaded-virtual
            -Wconversion
            -Wsign-conversion
            -Wnull-dereference
            -Wdouble-promotion
            -Wformat=2
            -Wimplicit-fallthrough
            -Wno-unknown-pragmas
            PARENT_SCOPE
        )
    elseif(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        set(${outvar}
            -Wall
            -Wextra
            -Werror
            -Wpedantic
            -Wshadow
            -Wnon-virtual-dtor
            -Wold-style-cast
            -Wcast-align
            -Wunused
            -Woverloaded-virtual
            -Wconversion
            -Wsign-conversion
            -Wnull-dereference
            -Wdouble-promotion
            -Wformat=2
            -Wimplicit-fallthrough
            -Wmisleading-indentation
            -Wduplicated-cond
            -Wduplicated-branches
            -Wlogical-op
            -Wuseless-cast
            -Wno-unknown-pragmas
            PARENT_SCOPE
        )
    else()
        set(${outvar} "" PARENT_SCOPE)
    endif()
endfunction()

# ──────────────────────────────────────────────────────────────────
# Public API
# ──────────────────────────────────────────────────────────────────

# Apply warning flags to a specific target
function(idhmfis_set_warnings target)
    _idhmfis_warning_flags(_flags)
    target_compile_options(${target} PRIVATE ${_flags})
endfunction()

# Apply warning flags globally (add_compile_options)
function(idhmfis_set_global_warnings)
    _idhmfis_warning_flags(_flags)
    add_compile_options(${_flags})
endfunction()
