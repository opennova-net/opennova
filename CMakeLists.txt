cmake_minimum_required(VERSION 3.16)
project(opennova LANGUAGES C CXX)

# Add libraries
add_subdirectory(libs/pcx)
add_subdirectory(libs/fnt)
add_subdirectory(libs/foliage)
add_subdirectory(libs/til)
add_subdirectory(libs/tpj)
add_subdirectory(libs/trn)
add_subdirectory(libs/cpt)
add_subdirectory(libs/env)
add_subdirectory(libs/threedi)
add_subdirectory(libs/bad)
add_subdirectory(libs/adm)
add_subdirectory(libs/pff)
add_subdirectory(libs/gameprofile)
add_subdirectory(libs/bfc1)
add_subdirectory(libs/cbin)
add_subdirectory(libs/def)
add_subdirectory(libs/scr)
add_subdirectory(libs/rtxt)
add_subdirectory(libs/ase)
add_subdirectory(libs/tdp)
add_subdirectory(libs/oed)
add_subdirectory(libs/renderer)
add_subdirectory(libs/terrain)
add_subdirectory(libs/vfs)
add_subdirectory(libs/resource_index)
add_subdirectory(libs/mission)

set(OPENNOVA_CORE_TARGETS
    opennova_pcx
    opennova_fnt
    opennova_foliage
    opennova_til
    opennova_tpj
    opennova_trn
    opennova_cpt
    opennova_env
    opennova_terrain
    opennova_threedi
    opennova_pff
    opennova_gameprofile
    opennova_bfc1
    opennova_cbin
    opennova_bad
    opennova_adm
    opennova_def
    opennova_scr
    opennova_rtxt
    opennova_ase
    opennova_tdp
    opennova_oed
    opennova_renderer
    opennova_vfs
    opennova_resource_index
    opennova_mission
)

# --------------------------------------------------------------------------
# Shared library for FFI (Python ctypes, etc.)
# Bundles the core static libs into a single .so / .dll.
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
        target_link_libraries(opennova_shared PRIVATE ${OPENNOVA_CORE_TARGETS})
        foreach(target IN LISTS OPENNOVA_CORE_TARGETS)
            target_link_options(opennova_shared PRIVATE "/WHOLEARCHIVE:${target}")
        endforeach()
    elseif(APPLE)
        # Apple's ld has no --whole-archive; force-load each static lib by path
        # so otherwise-unreferenced symbols (e.g. threedi write/free) survive
        # into the FFI dylib, matching the GNU --whole-archive behaviour below.
        target_link_libraries(opennova_shared PRIVATE ${OPENNOVA_CORE_TARGETS})
        foreach(target IN LISTS OPENNOVA_CORE_TARGETS)
            target_link_options(opennova_shared PRIVATE
                "LINKER:-force_load,$<TARGET_FILE:${target}>")
        endforeach()
    else()
        target_link_libraries(opennova_shared
            PRIVATE -Wl,--whole-archive
            PRIVATE ${OPENNOVA_CORE_TARGETS}
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
    foreach(target IN LISTS OPENNOVA_CORE_TARGETS)
        target_compile_definitions(${target} PRIVATE OPENNOVA_SHARED_EXPORTS)
        set_target_properties(${target} PROPERTIES POSITION_INDEPENDENT_CODE ON)
    endforeach()

    # Stage the built shared lib at the path blender/opennova/_native.py loads
    # from, so pytest can import the FFI without package_addon.sh having run.
    # blender/lib/**/*.{dll,so} are gitignored.
    if(CMAKE_SYSTEM_NAME STREQUAL "Windows")
        set(_opennova_blender_lib_subdir "windows-x64")
        set(_opennova_blender_lib_filename "opennova.dll")
    elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        set(_opennova_blender_lib_subdir "linux-x64")
        set(_opennova_blender_lib_filename "libopennova.so")
    elseif(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
        if(CMAKE_SYSTEM_PROCESSOR STREQUAL "arm64")
            set(_opennova_blender_lib_subdir "macos-arm64")
        else()
            set(_opennova_blender_lib_subdir "macos-x64")
        endif()
        set(_opennova_blender_lib_filename "libopennova.dylib")
    endif()

    if(_opennova_blender_lib_subdir)
        set(_opennova_blender_lib_dir
            "${CMAKE_SOURCE_DIR}/blender/lib/${_opennova_blender_lib_subdir}")
        add_custom_command(TARGET opennova_shared POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E make_directory "${_opennova_blender_lib_dir}"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "$<TARGET_FILE:opennova_shared>"
                "${_opennova_blender_lib_dir}/${_opennova_blender_lib_filename}"
            VERBATIM)
    endif()
endif()

option(OPENNOVA_ENABLE_MODSUPEROED_TOOLS "Build Windows x86 ModSuperOED automation tools when supported" ON)

if(OPENNOVA_ENABLE_MODSUPEROED_TOOLS)
    if(WIN32 AND CMAKE_SIZEOF_VOID_P EQUAL 4)
        add_subdirectory(tools/modsuperoed)
    elseif(WIN32)
        message(STATUS "Skipping ModSuperOED automation tools: a 32-bit Windows toolchain is required")
    else()
        message(STATUS "Skipping ModSuperOED automation tools: Windows is required")
    endif()
endif()

# Enable testing
enable_testing()
add_subdirectory(tests)

option(OPENNOVA_ENABLE_PYTHON_TESTS "Add Python pytest suite to CTest" OFF)

if(OPENNOVA_ENABLE_PYTHON_TESTS)
    find_program(UV_EXECUTABLE NAMES uv)
    if(NOT UV_EXECUTABLE)
        message(FATAL_ERROR "OPENNOVA_ENABLE_PYTHON_TESTS requires uv on PATH")
    endif()
    set(OPENNOVA_UV_CACHE_DIR "${CMAKE_CURRENT_BINARY_DIR}/uv-cache")
    set(OPENNOVA_PYTEST_COMMAND
        ${CMAKE_COMMAND} -E env
        "UV_CACHE_DIR=${OPENNOVA_UV_CACHE_DIR}"
        ${UV_EXECUTABLE} run --frozen pytest
    )

    add_test(
        NAME opennova_python_pytest
        COMMAND ${OPENNOVA_PYTEST_COMMAND}
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    )

    add_custom_target(opennova_python_tests
        COMMAND
            ${CMAKE_COMMAND} -E chdir
            "${CMAKE_SOURCE_DIR}"
            ${OPENNOVA_PYTEST_COMMAND}
        USES_TERMINAL
    )
endif()
