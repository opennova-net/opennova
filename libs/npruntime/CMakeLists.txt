add_library(opennova_npruntime STATIC
    src/server_session.cpp
    src/napi_np_protocol.cpp
    src/joiner_connection.cpp
    src/server_spawn.cpp
    src/server_initial_state.cpp
)

target_include_directories(opennova_npruntime
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

# The IDA-faithful in-match NAPI NP runtime (the named server/client state machines + frame
# loop). Builds on the netsim seam (world <-> wire bridge + transport) and the wire codecs.
# Strictly Godot-free and socket-free: the owner (apps/nw_server or the Godot binding) pumps
# bytes through netsim::ISessionTransport.
# opennova_novaworld is already transitive via opennova_netsim; name it explicitly because P2's
# server legs use its codecs + GameServerRuntime directly (direct-dependency hygiene). No cycle:
# novaworld does not depend on npruntime.
# opennova_world / opennova_mission are transitive via opennova_netsim; name them explicitly because
# P3's spawn pipeline + initial-state burst use world::spawn_player / select_player_spawn and
# bms::encode_header_blob directly (direct-dependency hygiene). No cycle: neither depends on npruntime.
target_link_libraries(opennova_npruntime PUBLIC
    opennova_netsim opennova_novaworld opennova_world opennova_mission)

target_compile_features(opennova_npruntime PUBLIC cxx_std_17)

set_target_properties(opennova_npruntime PROPERTIES
    POSITION_INDEPENDENT_CODE ON
)
