# nw_pp — NovaWorld in-game packet pretty-printer.
#
# Sibling RE / sanity-check tool: takes a pcap/pcapng, .sph server log, or
# hexcap (the same format `nw_ingame_histogram_test` and
# `nw_ingame_pool_records_test` consume) and prints each inner protocol message
# in human-readable form, with full field expansion for the tags whose §5.x
# field maps are landed.
#
# Reuses the same outer-decode pipeline (napi envelope + novacrypto NWU +
# novaworld protocol) so the bytes shown are exactly what the shipping libs
# parse — no parallel re-implementation.

add_executable(nw_pp nw_pp.cpp)

target_link_libraries(nw_pp
    PRIVATE
        opennova_npwire
        opennova_napi
        opennova_novacrypto
        opennova_def
        opennova_scr
        opennova_app_common
 opennova_pcapio)

target_compile_features(nw_pp PRIVATE cxx_std_17)
