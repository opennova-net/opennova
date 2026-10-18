# opennova-wire — NovaWorld in-game packet pretty-printer.
#
# Sibling RE / sanity-check tool: takes a pcap/pcapng, .sph server log, or
# hexcap (one `<srcport> <frame> <hex>` datagram per line) and prints each
# inner protocol message
# in human-readable form, with full field expansion for the tags whose §5.x
# field maps are landed.
#
# Reuses the same outer-decode pipeline (napi envelope + novacrypto NWU +
# novaworld protocol) so the bytes shown are exactly what the shipping libs
# parse — no parallel re-implementation.

# The canonical parity trace (`--parity-events`, ADR 0050 R4) and the items.def
# tables the decoders key on: opennova-wire prints the trace, opennova-parity
# (apps/parity) compares it, so both link this one builder.
add_library(opennova_wire_trace STATIC
    item_tables.cpp
    parity_trace.cpp
)

target_include_directories(opennova_wire_trace
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(opennova_wire_trace
    PUBLIC
        opennova_net
        opennova_formats
        opennova_base
)

target_compile_features(opennova_wire_trace PUBLIC cxx_std_17)

add_executable(opennova_wire main.cpp scenario_events.cpp)

# The wire group for the decode, and the runtime group for the one HUD policy
# it prints beside the bytes (the 0x1E feed kind -> STRCND key table,
# runtime/hud/feed_format).
target_link_libraries(opennova_wire
    PRIVATE
        opennova_wire_trace
        opennova_runtime
        opennova_net
        opennova_formats
        opennova_base)

target_compile_features(opennova_wire PRIVATE cxx_std_17)
set_target_properties(opennova_wire PROPERTIES OUTPUT_NAME opennova-wire)

# The capture and items.def paths arrive as arguments: without a UTF-8 manifest Windows hands
# them over in the ANSI code page, and the engine opens files from UTF-8 strings (io/os_path.h),
# so a non-ASCII path fails to open. The same manifest as opennova-3di (MSVC merges a .manifest
# source into the one it embeds).
if(MSVC)
    target_sources(opennova_wire PRIVATE ${CMAKE_SOURCE_DIR}/apps/3di/utf8.manifest)
endif()
