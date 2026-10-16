# opennova-update-tool: the update service's tooling (ADR 0061): builds a packed update (1SFS)
# from two install trees, inspects one, and serves packed updates over UPDATE.EXE's protocol with
# no database. The formats and the wire are the engine's (formats/update, net/update); the app
# owns the tree diff, the package catalog, the sockets (apps/common) and the console.
#
# The core is a STATIC library so the executable and the update_tool ctests drive the very same
# code.
add_library(opennova_update_tool_core STATIC
    package_builder.cpp   # two trees to a packed update
    package_catalog.cpp   # packed updates as the server's products
    update_tcp_server.cpp # the listeners and connections, pumped into net/update's machine
)

target_include_directories(opennova_update_tool_core
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(opennova_update_tool_core
    PUBLIC
        opennova_net         # the wire and the server machine (net/update); formats below it
        opennova_app_common  # the TCP sockets (PUBLIC-links ws2_32 on Windows)
)

target_compile_features(opennova_update_tool_core PUBLIC cxx_std_17)

add_executable(opennova_update_tool main.cpp)
set_target_properties(opennova_update_tool PROPERTIES OUTPUT_NAME opennova-update-tool)
target_link_libraries(opennova_update_tool PRIVATE opennova_update_tool_core)
target_compile_features(opennova_update_tool PRIVATE cxx_std_17)

# The baseline and target trees and the package paths arrive as arguments: without a UTF-8
# manifest Windows hands them over in the ANSI code page, and the engine opens files from UTF-8
# strings (io/os_path.h), so a non-ASCII path fails to open. The same manifest as opennova-3di
# (MSVC merges a .manifest source into the one it embeds).
if(MSVC)
    target_sources(opennova_update_tool PRIVATE ${CMAKE_SOURCE_DIR}/apps/3di/utf8.manifest)
endif()
