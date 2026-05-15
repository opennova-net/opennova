cmake_minimum_required(VERSION 3.16)
project(opennova LANGUAGES C CXX)

# Add libraries
add_subdirectory(libs/pcx)
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
add_subdirectory(libs/bfc1)
add_subdirectory(libs/def)
add_subdirectory(libs/scr)
add_subdirectory(libs/ase)
add_subdirectory(libs/tdp)
add_subdirectory(libs/object)
add_subdirectory(libs/terrain)
add_subdirectory(libs/resource_index)

set(OPENNOVA_CORE_TARGETS
    opennova_pcx
    opennova_foliage
    opennova_til
    opennova_tpj
    opennova_trn
    opennova_cpt
    opennova_env
    opennova_terrain
    opennova_threedi
    opennova_pff
    opennova_bfc1
    opennova_bad
    opennova_adm
    opennova_def
    opennova_scr
    opennova_ase
    opennova_tdp
    opennova_object
    opennova_resource_index
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

    # Stage the built shared lib next to pyopennova/_native.py so pytest can
    # import the FFI without package_addon.sh having run.
    # pyopennova/lib/**/*.{dll,so} are gitignored.
    if(CMAKE_SYSTEM_NAME STREQUAL "Windows")
        set(_opennova_pkg_lib_subdir "windows-x64")
        set(_opennova_pkg_lib_filename "opennova.dll")
    elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        set(_opennova_pkg_lib_subdir "linux-x64")
        set(_opennova_pkg_lib_filename "libopennova.so")
    endif()

    if(_opennova_pkg_lib_subdir)
        set(_opennova_pkg_lib_dir
            "${CMAKE_SOURCE_DIR}/pyopennova/lib/${_opennova_pkg_lib_subdir}")
        add_custom_command(TARGET opennova_shared POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E make_directory "${_opennova_pkg_lib_dir}"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "$<TARGET_FILE:opennova_shared>"
                "${_opennova_pkg_lib_dir}/${_opennova_pkg_lib_filename}"
            VERBATIM)
    endif()
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

option(OPENNOVA_ENABLE_LEGACY_OED_TOOLS "Build Windows-only legacy OED automation tools" OFF)

if(OPENNOVA_ENABLE_LEGACY_OED_TOOLS)
    add_subdirectory(tools/legacy_oed/modsuperoed)
endif()

# Enable testing
enable_testing()
add_subdirectory(tests)

find_program(UV_EXECUTABLE NAMES uv)
if(NOT UV_EXECUTABLE)
    message(FATAL_ERROR "uv is required to register the Python pytest suite")
endif()
set(OPENNOVA_PYTEST_COMMAND
    ${UV_EXECUTABLE} run --frozen --group blender pytest
    -p no:cacheprovider
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
