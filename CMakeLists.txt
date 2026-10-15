cmake_minimum_required(VERSION 3.16)
project(opennova LANGUAGES C CXX)

# FetchContent populates in the configure step itself (CMake 3.30+) instead of
# configuring and building a sub-project per dependency on every configure.
if(POLICY CMP0168)
    cmake_policy(SET CMP0168 NEW)
endif()

# The NovaWorld service (the server-side lobby and persistence leg: the
# opennova_novaworld_service target, apps/novaworld_server and their tests)
# builds only when asked. It runs on Linux (the server image, the Linux CI
# jobs) over PostgreSQL through the system libpq (libpq-dev to build, libpq5
# to run); the game, the tools and opennova-serve never need it.
option(OPENNOVA_NOVAWORLD_SERVICE
    "Build the NovaWorld service (opennova_novaworld_service, apps/novaworld_server, their tests)" OFF)
# Its HTTP layer: Crow and Asio, fetched at configure time.
option(BUILD_NOVAWORLD_HTTP "Fetch + build Crow for the novaworld HTTP layer" OFF)
if(BUILD_NOVAWORLD_HTTP AND NOT OPENNOVA_NOVAWORLD_SERVICE)
    message(FATAL_ERROR "BUILD_NOVAWORLD_HTTP is the NovaWorld service's HTTP layer: "
        "configure with -DOPENNOVA_NOVAWORLD_SERVICE=ON as well")
endif()

# Vendored dependencies used by the native network stack.
add_subdirectory(third_party/bcrypt)
# Vendored miniz: the BFC1 container decoder's inflate (engine/formats/bfc1).
add_subdirectory(third_party/miniz)
# rgbcx: the authoring DXT encoder the tools write with (the image import, opennova-3di
# texture: engine/runtime/renderer/dxt_encode).
add_subdirectory(third_party/rgbcx)

# The game dev tools and Dear ImGui are optional (ADR 0039). The root ctest
# build enables them by default; release GDExtensions compile them out.
option(OPENNOVA_DEVTOOLS "Build the game's in-engine dev-tool windows (Dear ImGui)" ON)
if(OPENNOVA_DEVTOOLS)
    add_subdirectory(third_party/imgui)
endif()

# Native engine group libraries.
add_subdirectory(engine)

# Optional standalone HTTP frontend for the novaworld server.
if(BUILD_NOVAWORLD_HTTP)
    include(FetchContent)
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

add_subdirectory(apps/common)
add_subdirectory(apps/3di)
add_subdirectory(apps/extract)
add_subdirectory(apps/lan_probe)
if(OPENNOVA_NOVAWORLD_SERVICE)
    add_subdirectory(apps/novaworld_server)
endif()
add_subdirectory(apps/parity)
add_subdirectory(apps/serve)
add_subdirectory(apps/wire)

enable_testing()
add_subdirectory(tests)
