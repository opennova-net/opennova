# opennova-novaworld-data: the NovaWorld service's data program
# (apps/novaworld_common/README.md "Programs"), built with the HTTP layer
# (BUILD_NOVAWORLD_HTTP). Its main.cpp over the data sets' library and the
# layers below it, nothing of hosting, billing or the accounts: the job pool
# over the data sets' jobs and the scheduler of their prune.
add_executable(opennova_novaworld_data main.cpp)
# The service directly (scripts/lint/link_graph_check.py: the libpq
# containment).
target_link_libraries(opennova_novaworld_data
    PRIVATE
        opennova_novaworld_data_sets
        opennova_novaworld_jobs
        opennova_novaworld_program
        opennova_novaworld_core
        opennova_novaworld_service
)
target_compile_features(opennova_novaworld_data PRIVATE cxx_std_17)
set_target_properties(opennova_novaworld_data PROPERTIES OUTPUT_NAME "opennova-novaworld-data")
