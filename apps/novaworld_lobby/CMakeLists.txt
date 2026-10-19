# opennova-novaworld-lobby: the NovaWorld service's lobby program, one lobby
# instance (apps/novaworld_common/README.md "Programs"). Its own code builds as
# opennova_novaworld_lobby_core (the NovaWorld UDP listener), which the tests
# drive in-process; the executable is its main.cpp over it, the program
# runtime and the core (apps/novaworld_common).
add_library(opennova_novaworld_lobby_core STATIC nw_udp_listener.cpp)
target_include_directories(opennova_novaworld_lobby_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
# The service edge is direct (scripts/lint/link_graph_check.py's libpq
# containment), as on every target here; the runtime group for the in-match
# host session the JO listener hosts peers through (runtime/inmatch, ADR 0043
# d4).
target_link_libraries(opennova_novaworld_lobby_core
    PUBLIC
        opennova_novaworld_core
        opennova_novaworld_program
        opennova_novaworld_service
        opennova_runtime
)
target_compile_features(opennova_novaworld_lobby_core PUBLIC cxx_std_17)

add_executable(opennova_novaworld_lobby main.cpp)
target_link_libraries(opennova_novaworld_lobby
    PRIVATE
        opennova_novaworld_lobby_core
        opennova_novaworld_program
        opennova_novaworld_core
        opennova_novaworld_service
)
set_target_properties(opennova_novaworld_lobby PROPERTIES OUTPUT_NAME "opennova-novaworld-lobby")
