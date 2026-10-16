# opennova-hostd: the machine agent (README.md). On each machine of the hosted servers'
# fleet it runs the opennova-serve containers the NovaWorld service places on the machine's
# slots (docker; or, on a machine that is itself a container, as its own child processes),
# counts each slot's egress (iptables) and reports them over the agent
# protocol, apps/novaworld_common/hosted/agent_protocol.*, which it compiles itself (Crow-free
# and database-free) so it builds without the service. Linux only: it runs as root under
# systemd, shipped inside the opennova-serve image (apps/serve/Dockerfile). Every source but
# main.cpp builds as opennova_hostd_core, so tests/hostd drives the agent in-process.
if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    return()
endif()

# The agent's build, as it reports it to the service (the serve image passes the commit).
set(OPENNOVA_HOSTD_VERSION "dev" CACHE STRING "opennova-hostd's reported build")

add_library(opennova_hostd_core STATIC
    agent.cpp            # the bootstrap and the sync passes
    docker_runtime.cpp   # the containers through the docker CLI
    egress_counters.cpp  # each slot's egress: iptables owner-UID counters
    hostd_options.cpp    # the command line
    process_runtime.cpp  # the servers as child processes (a machine that is a container)
    reconcile.cpp        # what a sync does, decided as a pure function
    slot_files.cpp       # the slots' working directories
    ${CMAKE_SOURCE_DIR}/apps/novaworld_common/hosted/agent_protocol.cpp
)

target_include_directories(opennova_hostd_core
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
        # hosted/agent_protocol.h and hosted/slots.h, the service's half of the protocol
        ${CMAKE_SOURCE_DIR}/apps/novaworld_common
)

target_compile_definitions(opennova_hostd_core PRIVATE
    OPENNOVA_HOSTD_VERSION="${OPENNOVA_HOSTD_VERSION}"
)

target_link_libraries(opennova_hostd_core
    PUBLIC
        opennova_app_common  # the command runner and the HTTP exchange (opennova_net, opennova_base)
)

target_compile_features(opennova_hostd_core PUBLIC cxx_std_17)

add_executable(opennova_hostd main.cpp)

target_link_libraries(opennova_hostd PRIVATE opennova_hostd_core)

target_compile_features(opennova_hostd PRIVATE cxx_std_17)
set_target_properties(opennova_hostd PROPERTIES OUTPUT_NAME "opennova-hostd")
