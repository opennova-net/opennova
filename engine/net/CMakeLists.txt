# The net stack (Shape A): two STATIC targets along the one seam that survives
# at target level.
#
#   opennova_net — everything a game endpoint needs: novacrypto, napi, the
#     npwire game wire codec, the novaworld SESSION and GATE legs (client-side
#     matchmaking), the netsim world<->wire seam, and the npruntime in-match
#     runtime. The godot/src links THIS target — never the service.
#
#   opennova_novaworld_service — the server-side lobby/persistence leg of
#     novaworld. GUARDRAIL (non-negotiable): sqlite containment is
#     linker-enforced — this is the ONLY target in the tree that links
#     opennova_sqlite (scripts/lint/link_graph_check.py pins it). Consumers:
#     apps/novaworld_server and the service tests.
#
# Wire compatibility holds for every protocol lib exactly as before
# (engine/CLAUDE.md; witness record docs/net/novaworld-net-re.md; ADRs
# 0009-0012, 0019). Headers keep their historical include shapes.

add_library(opennova_net STATIC
    # novacrypto — NWU crypto, URL cipher, pubcrypto, EPASK
    ${CMAKE_CURRENT_SOURCE_DIR}/novacrypto/src/url_cipher.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/novacrypto/src/nwu.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/novacrypto/src/pubcrypto.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/novacrypto/src/epask.cpp
    # napi — the NAPI envelope/TLV/session primitives
    ${CMAKE_CURRENT_SOURCE_DIR}/napi/src/envelope.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/napi/src/literal.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/napi/src/tlv.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/napi/src/session.cpp
    # npwire — the NovaLogic game wire protocol (ADR 0019): in-game codec, NWU
    # session framing, capture decode
    ${CMAKE_CURRENT_SOURCE_DIR}/npwire/src/wire/entity_class.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/npwire/src/wire/ingame_decode.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/npwire/src/wire/ingame_encode.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/npwire/src/session/session_hello.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/npwire/src/session/nw_session_framing.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/npwire/src/session/protocol_message.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/npwire/src/replay/wire_capture.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/npwire/src/replay/serverlog_decode.cpp
    # novaworld session leg — NWU session establishment above the npwire
    # framing: the client session state machine, the session-protocol
    # classifier, join identity, the lobby CU var-list codec
    ${CMAKE_CURRENT_SOURCE_DIR}/novaworld/src/session/session_protocol.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/novaworld/src/session/client_session.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/novaworld/src/session/join_identity.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/novaworld/src/session/lobby_vars.cpp
    # novaworld gate leg — first-contact + account entry: novaworld_gate
    # probe/response, registration URL cipher, HTTP login flow, GSB browser data
    ${CMAKE_CURRENT_SOURCE_DIR}/novaworld/src/gate/gate_probe.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/novaworld/src/gate/gate_response.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/novaworld/src/gate/registration_url.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/novaworld/src/gate/http_login.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/novaworld/src/gate/http_flow.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/novaworld/src/gate/gsb.cpp
    # netsim — the in-match net seam (ADR 0009/0011): NetSystem + the
    # World<->wire bridge + the in-process loopback. The single deliberate
    # bridge between the runtime world and the wire codec.
    ${CMAKE_CURRENT_SOURCE_DIR}/netsim/src/loopback_channel.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/netsim/src/serializing_sink.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/netsim/src/entity_wire_bridge.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/netsim/src/item_replication_catalog.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/netsim/src/client_replica_body_arbitration.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/netsim/src/client_replica_pipeline.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/netsim/src/client_world_materializer.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/netsim/src/connection_fan.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/netsim/src/udp_session_transport.cpp
    # npruntime — the IDA-faithful in-match NAPI NP runtime (the named
    # server/client state machines + frame loop). Godot-free and socket-free:
    # the owner (apps/nw_server, the Godot binding) pumps bytes through
    # netsim::ISessionTransport.
    ${CMAKE_CURRENT_SOURCE_DIR}/npruntime/src/server_session.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/npruntime/src/napi_np_protocol.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/npruntime/src/server_message_dispatch.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/npruntime/src/session_status.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/npruntime/src/charattr_challenge.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/npruntime/src/integrity_challenge_profile.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/npruntime/src/join_session_policy.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/npruntime/src/joiner_connection.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/npruntime/src/server_spawn.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/npruntime/src/server_initial_state.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/npruntime/src/server_tick.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/npruntime/src/client_runtime.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/npruntime/src/client_replica_present_projection.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/npruntime/src/loadout_submit.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/npruntime/src/joiner_world_bridge.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/npruntime/src/host_session.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/npruntime/src/lan_discovery.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/npruntime/src/weapon_table_build.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/npruntime/src/ammo_table_build.cpp
)

target_include_directories(opennova_net
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/napi/include
        ${CMAKE_CURRENT_SOURCE_DIR}/netsim/include
        ${CMAKE_CURRENT_SOURCE_DIR}/novacrypto/include
        ${CMAKE_CURRENT_SOURCE_DIR}/novaworld/include
        ${CMAKE_CURRENT_SOURCE_DIR}/npruntime/include
        ${CMAKE_CURRENT_SOURCE_DIR}/npwire/include
)

# The Shape A chain: net -> runtime -> base -> formats -> io (all PUBLIC).
# netsim/npruntime consume world/mission_format/def through it.
target_link_libraries(opennova_net PUBLIC opennova_runtime)

target_compile_features(opennova_net PUBLIC cxx_std_17)

set_target_properties(opennova_net PROPERTIES
    POSITION_INDEPENDENT_CODE ON
)

# service — the server-side lobby/persistence leg: lobby sessions and updates,
# host repository, connection registry/manager, unknown tracker, sqlite. The
# ONLY target that links sqlite (see the guardrail note at the top).
add_library(opennova_novaworld_service STATIC
    ${CMAKE_CURRENT_SOURCE_DIR}/novaworld/src/service/lobby_session.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/novaworld/src/service/lobby_update.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/novaworld/src/service/host_repository.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/novaworld/src/service/unknown_tracker.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/novaworld/src/service/connection/registry.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/novaworld/src/service/connection/manager.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/novaworld/src/service/db/sqlite.cpp
)

target_link_libraries(opennova_novaworld_service
    PUBLIC opennova_net
    PRIVATE opennova_sqlite
)

target_compile_features(opennova_novaworld_service PUBLIC cxx_std_17)

set_target_properties(opennova_novaworld_service PROPERTIES
    POSITION_INDEPENDENT_CODE ON
)
