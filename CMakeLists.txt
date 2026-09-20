cmake_minimum_required(VERSION 3.16)
project(opennova LANGUAGES C CXX)

# Vendored dependencies used by the native network stack.
add_subdirectory(third_party/sqlite)
add_subdirectory(third_party/bcrypt)
# Vendored miniz: the BFC1 container decoder's inflate (engine/formats/bfc1).
add_subdirectory(third_party/miniz)

# The game dev tools and Dear ImGui are optional (ADR 0039). The root ctest
# build enables them by default; release GDExtensions compile them out.
option(OPENNOVA_DEVTOOLS "Build the game's in-engine dev-tool windows (Dear ImGui)" ON)
if(OPENNOVA_DEVTOOLS)
    add_subdirectory(third_party/imgui)
endif()

# Native engine group libraries.
add_subdirectory(engine)

# Optional standalone HTTP frontend for the novaworld server.
option(BUILD_NOVAWORLD_HTTP "Fetch + build Crow for the novaworld HTTP layer" OFF)
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
add_subdirectory(apps/nw_lan_probe)
add_subdirectory(apps/novaworld_server)
add_subdirectory(apps/nw_server)
add_subdirectory(apps/nw_pp)
add_subdirectory(apps/extract)
add_subdirectory(apps/project)

enable_testing()
add_subdirectory(tests)
