# opennova-novaworld-gate: the NovaWorld service's gate program
# (apps/novaworld_common/README.md "Programs"). Its own code builds as
# opennova_novaworld_gate_core (the gate listener), which the tests drive
# in-process; the executable is its main.cpp over it and the shared library.
add_library(opennova_novaworld_gate_core STATIC gate_listener.cpp)
target_include_directories(opennova_novaworld_gate_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
# The service edge is direct (scripts/lint/link_graph_check.py's libpq
# containment), as on every target here.
target_link_libraries(opennova_novaworld_gate_core
    PUBLIC
        opennova_novaworld_common
        opennova_novaworld_service
)
target_compile_features(opennova_novaworld_gate_core PUBLIC cxx_std_17)

add_executable(opennova_novaworld_gate main.cpp)
target_link_libraries(opennova_novaworld_gate
    PRIVATE
        opennova_novaworld_gate_core
        opennova_novaworld_common
        opennova_novaworld_service
)
set_target_properties(opennova_novaworld_gate PROPERTIES OUTPUT_NAME "opennova-novaworld-gate")
