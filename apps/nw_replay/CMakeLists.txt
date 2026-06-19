# nw_replay — plays a captured NovaWorld session at N Godot game-client instances.
#
# Partitions the pcap by UDP port into roles (host + clients), assigns a role per
# instance as it dials in, and paces each role's RAW captured datagrams to it.
# The data plane is pure captured bytes (the Godot client decodes them like a real
# server's stream); only role assignment is bespoke. Reuses apps/common's
# pcap_reader + net_sockets and libs/novaworld's decode (for --validate).

add_executable(nw_replay
    nw_replay.cpp
    nw_replay_partition.cpp
)

target_link_libraries(nw_replay
    PRIVATE
        opennova_novaworld
        opennova_napi
        opennova_novacrypto
        opennova_def
        opennova_scr
        opennova_app_common
)

target_compile_features(nw_replay PRIVATE cxx_std_17)
