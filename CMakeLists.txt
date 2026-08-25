cmake_minimum_required(VERSION 3.16)
project(opennova LANGUAGES C CXX)

# Vendored third-party deps that the net stack pulls in. sqlite must exist
# before engine/ — the novaworld service target links it.
add_subdirectory(third_party/sqlite)
add_subdirectory(third_party/bcrypt)

# The engine (Shape A): five STATIC group targets + one header-only INTERFACE
# target. See engine/CMakeLists.txt for the group map and the PUBLIC link
# chain (formats -> io; base -> formats; runtime -> base; net -> runtime;
# novaworld_service -> net + sqlite).
add_subdirectory(engine)

# Crow — async C++ HTTP framework for the standalone novaworld server.
# Header-only; needs standalone Asio. Off by default to keep the lib-only /
# Godot-only build paths fast; the Linux server build + deploy image turn it on.
option(BUILD_NOVAWORLD_HTTP "Fetch + build Crow for the novaworld HTTP layer" OFF)
if(BUILD_NOVAWORLD_HTTP)
    include(FetchContent)

    # Standalone Asio — Crow requires it but doesn't bundle it in 1.2.x.
    # Asio is header-only; we just need to expose ASIO_INCLUDE_DIR for
    # Crow's Findasio.cmake to pick up.
    FetchContent_Declare(asio
        GIT_REPOSITORY https://github.com/chriskohlhoff/asio.git
        GIT_TAG        asio-1-30-2
        GIT_SHALLOW    TRUE
    )
    FetchContent_MakeAvailable(asio)
    set(ASIO_INCLUDE_DIR "${asio_SOURCE_DIR}/asio/include" CACHE PATH "" FORCE)

    set(CROW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(CROW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(CROW_INSTALL OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(crow
        GIT_REPOSITORY https://github.com/CrowCpp/Crow.git
        GIT_TAG        v1.2.0
        GIT_SHALLOW    TRUE
    )
    FetchContent_MakeAvailable(crow)
endif()

# Shared app utilities (UDP sockets, pcap writer) used by the standalone
# servers and the loopback tests.
add_subdirectory(apps/common)

# nw_lan_probe -- a bounded app-layer readiness probe for the retail-compatible
# 0x41/0x81 LAN discovery endpoint. Protocol bytes stay in npruntime; this app
# owns only the real UDP socket and retry/deadline policy.
add_subdirectory(apps/nw_lan_probe)

# Standalone novaworld server — gate UDP + NW UDP (+ HTTP when BUILD_NOVAWORLD_HTTP).
add_subdirectory(apps/novaworld_server)

# nw_server — the headless in-match game HOST (P6): a real-UDP owner loop over engine/net/npruntime
# (Server_TickUpdate + handle_server_datagram), separate from the matchmaking novaworld_server.
# Pure C++ — never links godot-cpp (it is not in this CMake graph; see apps/nw_server/CMakeLists.txt).
add_subdirectory(apps/nw_server)

# nw_pp — in-game packet pretty-printer for RE sanity-check (reads pcap/pcapng,
# .sph server logs, or `NW_INGAME_HEXCAP`-style hexcap and dumps decoded tags).
add_subdirectory(apps/nw_pp)

# The whole-archived FFI surface: the three sim-side groups. The net stack and
# the novaworld service are NOT bundled into the FFI shared lib (ADR 0019 §5 —
# abi_export_identity pins that no net symbol family appears). Whole-archiving
# a group carries members that were never in the old CORE list; that is fine —
# only OPENNOVA_API-annotated symbols export (hidden-by-default visibility +
# MSVC dllexport), and none of those members declare any.
set(OPENNOVA_CORE_TARGETS
    opennova_formats
    opennova_base
    opennova_runtime
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
        # GNU ld: force-load each core archive by FILE PATH (like the APPLE
        # branch). Referencing $<TARGET_FILE:...> instead of target names keeps
        # CMake's link-line de-duplication from pulling a dependency out of the
        # whole-archive region, so libraries may declare their real dependencies
        # (e.g. oed/renderer -> threedi) without losing exported symbols.
        target_link_libraries(opennova_shared PRIVATE ${OPENNOVA_CORE_TARGETS})
        foreach(target IN LISTS OPENNOVA_CORE_TARGETS)
            target_link_options(opennova_shared PRIVATE
                "LINKER:--whole-archive,$<TARGET_FILE:${target}>,--no-whole-archive")
        endforeach()
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
    # when their object files are linked into the shared library.
    # Hidden default visibility makes ELF/Mach-O match the PE surface: only
    # OPENNOVA_API-annotated symbols export, so the flat C ABI is identical on
    # every platform (abi_export_identity pins it to the committed baseline).
    foreach(target IN LISTS OPENNOVA_CORE_TARGETS)
        target_compile_definitions(${target} PRIVATE OPENNOVA_SHARED_EXPORTS)
        set_target_properties(${target} PROPERTIES
            POSITION_INDEPENDENT_CODE ON
            C_VISIBILITY_PRESET hidden
            CXX_VISIBILITY_PRESET hidden
            VISIBILITY_INLINES_HIDDEN ON)
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
