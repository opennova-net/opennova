cmake_minimum_required(VERSION 3.16)
project(opennova LANGUAGES C CXX)

# Add libraries
add_subdirectory(libs/threedi)
add_subdirectory(libs/bad)
add_subdirectory(libs/adm)
add_subdirectory(libs/pff)
add_subdirectory(libs/bfc1)
add_subdirectory(libs/def)
add_subdirectory(libs/scr)
add_subdirectory(libs/ase)
add_subdirectory(libs/tdp)

# --------------------------------------------------------------------------
# Shared library for FFI (Python ctypes, etc.)
# Bundles threedi IR + BAD + ADM into a single .so / .dll
# --------------------------------------------------------------------------
option(BUILD_SHARED_LIB "Build the opennova shared library for FFI" OFF)

if(BUILD_SHARED_LIB)
    # Empty source list — all symbols come from the static libs via --whole-archive.
    # CMake requires at least one source, so we use an empty generated file.
    file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/empty.c" "")
    add_library(opennova_shared SHARED
        "${CMAKE_CURRENT_BINARY_DIR}/empty.c"
    )

    # Use --whole-archive to include all symbols from static libs
    # (the linker would otherwise discard unreferenced threedi symbols)
    if(MSVC)
        target_link_libraries(opennova_shared
            PRIVATE opennova_threedi opennova_pff opennova_bfc1 oed_bad oed_adm oed_def oed_scr opennova_ase opennova_tdp
        )
        target_link_options(opennova_shared PRIVATE
            /WHOLEARCHIVE:opennova_threedi
            /WHOLEARCHIVE:opennova_pff
            /WHOLEARCHIVE:oed_bad
            /WHOLEARCHIVE:oed_adm
            /WHOLEARCHIVE:oed_def
            /WHOLEARCHIVE:oed_scr
            /WHOLEARCHIVE:opennova_ase
            /WHOLEARCHIVE:opennova_tdp
        )
    else()
        target_link_libraries(opennova_shared
            PRIVATE -Wl,--whole-archive
            PRIVATE opennova_threedi opennova_pff opennova_bfc1 oed_bad oed_adm oed_def oed_scr opennova_ase opennova_tdp
            PRIVATE -Wl,--no-whole-archive
        )
    endif()

    target_compile_definitions(opennova_shared
        PRIVATE OPENNOVA_SHARED_EXPORTS
    )

    set_target_properties(opennova_shared PROPERTIES
        OUTPUT_NAME "opennova"
        C_VISIBILITY_PRESET hidden
        VISIBILITY_INLINES_HIDDEN ON
        POSITION_INDEPENDENT_CODE ON
    )

    # Statically link the C/C++ runtimes so the DLL has no MinGW dependencies
    if(MINGW)
        target_link_options(opennova_shared PRIVATE -static-libgcc -static-libstdc++)
    endif()

    # Static libs need OPENNOVA_SHARED_EXPORTS so their functions get dllexport
    # when their object files are linked into the shared library
    target_compile_definitions(opennova_threedi PRIVATE OPENNOVA_SHARED_EXPORTS)
    target_compile_definitions(opennova_pff PRIVATE OPENNOVA_SHARED_EXPORTS)
    target_compile_definitions(oed_bad PRIVATE OPENNOVA_SHARED_EXPORTS)
    target_compile_definitions(oed_adm PRIVATE OPENNOVA_SHARED_EXPORTS)
    target_compile_definitions(oed_def PRIVATE OPENNOVA_SHARED_EXPORTS)
    target_compile_definitions(opennova_bfc1 PRIVATE OPENNOVA_SHARED_EXPORTS)
    target_compile_definitions(oed_scr PRIVATE OPENNOVA_SHARED_EXPORTS)
    target_compile_definitions(opennova_ase PRIVATE OPENNOVA_SHARED_EXPORTS)
    target_compile_definitions(opennova_tdp PRIVATE OPENNOVA_SHARED_EXPORTS)

    # On Linux the static libs must be compiled with -fPIC
    set_target_properties(opennova_threedi PROPERTIES POSITION_INDEPENDENT_CODE ON)
    set_target_properties(opennova_pff PROPERTIES POSITION_INDEPENDENT_CODE ON)
    set_target_properties(oed_bad PROPERTIES POSITION_INDEPENDENT_CODE ON)
    set_target_properties(oed_adm PROPERTIES POSITION_INDEPENDENT_CODE ON)
    set_target_properties(oed_def PROPERTIES POSITION_INDEPENDENT_CODE ON)
    set_target_properties(opennova_bfc1 PROPERTIES POSITION_INDEPENDENT_CODE ON)
    set_target_properties(oed_scr PROPERTIES POSITION_INDEPENDENT_CODE ON)
    set_target_properties(opennova_ase PROPERTIES POSITION_INDEPENDENT_CODE ON)
    set_target_properties(opennova_tdp PROPERTIES POSITION_INDEPENDENT_CODE ON)
endif()

# --------------------------------------------------------------------------
# Development tools (3di_dump, etc.)
# --------------------------------------------------------------------------
option(BUILD_TOOLS "Build development tools" OFF)

if(BUILD_TOOLS)
    add_executable(3di_dump tools/3di_dump.cpp)
    target_link_libraries(3di_dump PRIVATE opennova_threedi m)

    add_executable(survey_3di3 tools/survey_3di3.cpp)
    target_link_libraries(survey_3di3 PRIVATE opennova_threedi m)
endif()

# Enable testing
enable_testing()
add_subdirectory(tests)
