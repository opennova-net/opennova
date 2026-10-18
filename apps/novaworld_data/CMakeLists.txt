# opennova-novaworld-data: the NovaWorld service's data program
# (apps/novaworld_common/README.md "Programs"), built with the HTTP layer
# (BUILD_NOVAWORLD_HTTP, whose half of the shared library holds the data sets'
# jobs). Its main.cpp over the shared library: the job pool over the data sets'
# jobs and the scheduler of their prune.
add_executable(opennova_novaworld_data main.cpp)
# The shared library and the service directly (scripts/lint/link_graph_check.py:
# the hiredis and libpq containments).
target_link_libraries(opennova_novaworld_data
    PRIVATE
        opennova_novaworld_common
        opennova_novaworld_service
)
target_compile_features(opennova_novaworld_data PRIVATE cxx_std_17)
set_target_properties(opennova_novaworld_data PROPERTIES OUTPUT_NAME "opennova-novaworld-data")
