# nw_replay — streams a representative captured NovaWorld S2C path to a spectator.
#
# Partitions the pcap by UDP port into roles (host + clients), can print/validate
# the roles, and paces one representative host-to-client RAW captured stream to a
# spectator. The data plane is pure captured bytes (the Godot client decodes them
# like a real server's stream). Reuses apps/common's pcap_reader + net_sockets and
# libs/novaworld's decode (for --validate).

add_executable(nw_replay
    nw_replay.cpp
    nw_replay_cli.cpp
    nw_replay_partition.cpp
)

target_link_libraries(nw_replay
    PRIVATE
        opennova_npwire
        opennova_napi
        opennova_novacrypto
        opennova_def
        opennova_scr
        opennova_app_common
        opennova_pcapio)

target_compile_features(nw_replay PRIVATE cxx_std_17)
