# opennova-novaworld-worker: the NovaWorld service's worker program
# (apps/novaworld_common/README.md "Programs"), built with the HTTP layer
# (BUILD_NOVAWORLD_HTTP). Its periodic jobs build as
# opennova_novaworld_worker_core (worker_schedule.*), which web links too (the
# admin's jobs page shows and runs every program's from the same settings);
# its main.cpp over it and the service's libraries, the cloud's alone among
# the programs: the job pool, the handlers it registers, the machine driver
# it builds.
add_library(opennova_novaworld_worker_core STATIC worker_schedule.cpp)
target_include_directories(opennova_novaworld_worker_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
# The libraries it reads and the service directly (scripts/lint/link_graph_check.py:
# the libpq containment).
target_link_libraries(opennova_novaworld_worker_core
    PUBLIC
        opennova_novaworld_core
        opennova_novaworld_program
        opennova_novaworld_jobs
        opennova_novaworld_accounts
        opennova_novaworld_billing
        opennova_novaworld_hosting_model
        opennova_novaworld_service
)
target_compile_features(opennova_novaworld_worker_core PUBLIC cxx_std_17)

add_executable(opennova_novaworld_worker main.cpp)
target_link_libraries(opennova_novaworld_worker
    PRIVATE
        opennova_novaworld_worker_core
        opennova_novaworld_hosting_cloud
        opennova_novaworld_hosting_model
        opennova_novaworld_billing
        opennova_novaworld_accounts
        opennova_novaworld_jobs
        opennova_novaworld_program
        opennova_novaworld_core
        opennova_novaworld_service
)
target_compile_features(opennova_novaworld_worker PRIVATE cxx_std_17)
set_target_properties(opennova_novaworld_worker PROPERTIES OUTPUT_NAME "opennova-novaworld-worker")
