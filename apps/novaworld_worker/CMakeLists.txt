# opennova-novaworld-worker: the NovaWorld service's worker program
# (apps/novaworld_common/README.md "Programs"), built with the HTTP layer
# (BUILD_NOVAWORLD_HTTP, whose half of the shared library holds the jobs'
# handlers). Its main.cpp over the shared library: the job pool, the
# handlers it registers, the machine driver it builds.
add_executable(opennova_novaworld_worker main.cpp)
# The shared library and the service directly (scripts/lint/link_graph_check.py:
# the hiredis and libpq containments).
target_link_libraries(opennova_novaworld_worker
    PRIVATE
        opennova_novaworld_common
        opennova_novaworld_service
)
target_compile_features(opennova_novaworld_worker PRIVATE cxx_std_17)
set_target_properties(opennova_novaworld_worker PROPERTIES OUTPUT_NAME "opennova-novaworld-worker")
