# opennova-wire — NovaWorld in-game packet pretty-printer.
#
# Sibling RE / sanity-check tool: takes a pcap/pcapng, .sph server log, or
# hexcap (the committed `fixtures/novaworld/*.hexcap` format) and prints each
# inner protocol message
# in human-readable form, with full field expansion for the tags whose §5.x
# field maps are landed.
#
# Reuses the same outer-decode pipeline (napi envelope + novacrypto NWU +
# novaworld protocol) so the bytes shown are exactly what the shipping libs
# parse — no parallel re-implementation.

add_executable(opennova_wire main.cpp scenario_events.cpp)

# The wire group for the decode, and the runtime group for the one HUD policy
# it prints beside the bytes (the 0x1E feed kind -> STRCND key table,
# runtime/hud/feed_format).
target_link_libraries(opennova_wire
    PRIVATE
        opennova_runtime
        opennova_net
        opennova_formats
        opennova_base)

target_compile_features(opennova_wire PRIVATE cxx_std_17)
set_target_properties(opennova_wire PROPERTIES OUTPUT_NAME opennova-wire)
