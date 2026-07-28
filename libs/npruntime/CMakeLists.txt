add_library(opennova_npruntime STATIC
    src/server_session.cpp
    src/napi_np_protocol.cpp
    src/server_message_dispatch.cpp
    src/charattr_challenge.cpp
    src/joiner_connection.cpp
    src/server_spawn.cpp
    src/server_initial_state.cpp
    src/server_tick.cpp
    src/client_runtime.cpp
    src/host_session.cpp
    src/lan_discovery.cpp
    src/weapon_table_build.cpp
    src/ammo_table_build.cpp
)

target_include_directories(opennova_npruntime
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

# The IDA-faithful in-match NAPI NP runtime (the named server/client state machines + frame
# loop). Builds on the netsim seam (world <-> wire bridge + transport) and the wire codecs.
# Strictly Godot-free and socket-free: the owner (apps/nw_server or the Godot binding) pumps
# bytes through netsim::ISessionTransport.
# The npwire wire lib is transitive via opennova_netsim; the legs are named explicitly because the server
# legs + the reactive reply dispatcher use its codecs (protocol_message, ingame_decode, replication_model
# POD structs) directly (direct-dependency hygiene). No cycle: novaworld does not depend on npruntime.
# opennova_world is transitive via opennova_netsim; it and mission_format are named explicitly because
# P3's spawn pipeline + initial-state burst use world::spawn_player / select_player_spawn and
# bms::encode_header_blob directly (direct-dependency hygiene). No cycle: neither depends on npruntime.
# opennova_def: weapon_table_build parses DefWeaponsFile into the world armory table
# (D-NET-141); def is a leaf C library — no cycle.
# opennova_io: header-only strutil (ascii iequals) for the ammo effects-table bake.
target_link_libraries(opennova_npruntime PUBLIC
    opennova_netsim opennova_npwire opennova_world opennova_mission_format opennova_def opennova_io)

target_compile_features(opennova_npruntime PUBLIC cxx_std_17)

set_target_properties(opennova_npruntime PROPERTIES
    POSITION_INDEPENDENT_CODE ON
)
