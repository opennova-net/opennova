# opennova-novaworld-fleet: the NovaWorld service's fleet program
# (apps/novaworld_common/README.md "Programs"), built with the HTTP layer
# (BUILD_NOVAWORLD_HTTP). Its own code builds as opennova_novaworld_fleet_core
# (the election, the hosted servers' supervisor with its stats sampler, the
# admin links and the console drain over them, the internal listener and the
# machine agents' routes on it), which the tests drive in-process; the
# executable is its main.cpp over it and the shared library.
add_library(opennova_novaworld_fleet_core STATIC
    election.cpp
    hosted/admin_link.cpp
    hosted/agent_route.cpp
    hosted/console_drain.cpp
    hosted/internal_listener.cpp
    hosted/stats_sampler.cpp
    hosted/supervisor.cpp
)
target_include_directories(opennova_novaworld_fleet_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
# The shared library and the service directly (scripts/lint/link_graph_check.py:
# the libpq containment), as on every target here.
target_link_libraries(opennova_novaworld_fleet_core
    PUBLIC
        opennova_novaworld_common
        opennova_novaworld_service
)
target_compile_features(opennova_novaworld_fleet_core PUBLIC cxx_std_17)

add_executable(opennova_novaworld_fleet main.cpp)
target_link_libraries(opennova_novaworld_fleet
    PRIVATE
        opennova_novaworld_fleet_core
        opennova_novaworld_common
        opennova_novaworld_service
)
set_target_properties(opennova_novaworld_fleet PROPERTIES
    OUTPUT_NAME "opennova-novaworld-fleet"
)
