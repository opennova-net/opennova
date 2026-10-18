# opennova-hostd: the machine agent (README.md). On each machine of the hosted servers'
# fleet it runs the opennova-serve containers the NovaWorld service places on the machine's
# slots (docker; or, on a machine that is itself a container, as its own child processes),
# fences the slots' users and counts each slot's traffic (nftables) and reports them over
# the agent protocol, apps/novaworld_common/hosted/agent_protocol.*, which it compiles
# itself with the ECS metadata reader (Crow-free and database-free) so it builds without
# the service. Linux only: it runs as root, the fleet cluster's ECS daemon, from an image
# of its own (apps/serve/Dockerfile's hostd target). Every source but main.cpp builds as
# opennova_hostd_core, so tests/hostd drives the agent in-process.
if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    return()
endif()

# The agent's build, as it reports it to the service (the serve image passes the commit).
set(OPENNOVA_HOSTD_VERSION "dev" CACHE STRING "opennova-hostd's reported build")

add_library(opennova_hostd_core STATIC
    agent.cpp            # the bootstrap and the sync passes
    docker_runtime.cpp   # the containers through the docker CLI
    fence_probe.cpp      # the fences' self-test as a slot's user
    hostd_options.cpp    # the command line
    instance_metadata.cpp # the machine's image, from the instance metadata service
    machine_identity.cpp # the machine id file, the ECS attribute check, the alive file
    nft_table.cpp        # the slots' fences and counters: table inet opennova
    process_runtime.cpp  # the servers as child processes (a machine that is a container)
    reconcile.cpp        # what a sync does, decided as a pure function
    saved_machine.cpp    # the saved bootstrap, a format of the agent's own
    slot_files.cpp       # the slots' working directories
    tree_store.cpp       # the trees fetched on demand (aws s3 sync) and checked
    ${CMAKE_SOURCE_DIR}/apps/novaworld_common/hosted/agent_protocol.cpp
    ${CMAKE_SOURCE_DIR}/apps/novaworld_common/ecs_metadata.cpp  # its own image and log stream on ECS
    ${CMAKE_SOURCE_DIR}/apps/novaworld_common/proc_stats.cpp  # the machine's CPU, memory, disk, network
)

target_include_directories(opennova_hostd_core
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
        # hosted/agent_protocol.h and hosted/slots.h, the service's half of the protocol,
        # ecs_metadata.h and proc_stats.h
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
