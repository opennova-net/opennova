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
    ${CMAKE_CURRENT_SOURCE_DIR}/novacrypto/url_cipher.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/novacrypto/nwu.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/novacrypto/pubcrypto.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/novacrypto/epask.cpp
    # napi — the NAPI envelope/TLV/session primitives
    ${CMAKE_CURRENT_SOURCE_DIR}/napi/envelope.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/napi/literal.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/napi/tlv.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/napi/session.cpp
    # npwire — the NovaLogic game wire protocol (ADR 0019): in-game codec, NWU
    # session framing, capture decode
    ${CMAKE_CURRENT_SOURCE_DIR}/npwire/wire/entity_class.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/npwire/wire/ingame_decode.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/npwire/wire/ingame_encode.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/npwire/session/session_hello.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/npwire/session/nw_session_framing.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/npwire/session/protocol_message.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/npwire/replay/wire_capture.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/npwire/replay/serverlog_decode.cpp
    # novaworld session leg — NWU session establishment above the npwire
    # framing: the client session state machine, the session-protocol
    # classifier, join identity, the lobby CU var-list codec
    ${CMAKE_CURRENT_SOURCE_DIR}/novaworld/session/session_protocol.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/novaworld/session/client_session.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/novaworld/session/join_identity.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/novaworld/session/lobby_vars.cpp
    # novaworld gate leg — first-contact + account entry: novaworld_gate
    # probe/response, registration URL cipher, HTTP login flow, GSB browser data
    ${CMAKE_CURRENT_SOURCE_DIR}/novaworld/gate/gate_probe.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/novaworld/gate/gate_response.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/novaworld/gate/registration_url.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/novaworld/gate/http_login.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/novaworld/gate/http_flow.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/novaworld/gate/gsb.cpp
    # netsim — the in-match net seam (ADR 0009/0011): connection-fan
    # primitives, client replica state, the World<->wire bridge, and the
    # in-process loopback.
    ${CMAKE_CURRENT_SOURCE_DIR}/netsim/loopback_channel.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/netsim/serializing_sink.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/netsim/entity_wire_bridge.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/netsim/item_replication_catalog.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/netsim/client_replica_body_arbitration.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/netsim/client_replica_minimap.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/netsim/client_replica_body_ticks.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/netsim/client_replica_pipeline.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/netsim/client_replica_death.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/netsim/client_roster_tags.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/netsim/client_state.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/npruntime/stat_screen_feed.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/npruntime/minimap_markers.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/npruntime/character_registry.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/netsim/client_replica_placed_device.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/netsim/client_replica_guided.cpp  # the 0x44 guidance fold + client flight (D-NET-64)
    ${CMAKE_CURRENT_SOURCE_DIR}/netsim/client_replica_feed.cpp  # the S2C 0x1E feed fold
    ${CMAKE_CURRENT_SOURCE_DIR}/netsim/client_replica_scoreboard.cpp  # the 0x16/0x46 board lanes
    ${CMAKE_CURRENT_SOURCE_DIR}/netsim/client_replica_endround.cpp    # the chunked 0x56 stat board
    ${CMAKE_CURRENT_SOURCE_DIR}/netsim/client_scoreboard_view.cpp  # the draw-time board projection
    ${CMAKE_CURRENT_SOURCE_DIR}/netsim/client_world_materializer.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/netsim/connection_fan.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/netsim/udp_session_transport.cpp
    # npruntime — the IDA-faithful in-match NAPI NP runtime (the named
    # server/client state machines + frame loop). Godot-free and socket-free:
    # the owner (apps/nw_server, the Godot binding) pumps bytes through
    # netsim::ISessionTransport.
    ${CMAKE_CURRENT_SOURCE_DIR}/npruntime/server_session.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/npruntime/napi_np_protocol.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/npruntime/end_round_protocol.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/npruntime/server_message_dispatch.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/npruntime/session_status.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/npruntime/charattr_challenge.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/npruntime/integrity_challenge_profile.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/npruntime/join_session_policy.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/npruntime/joiner_connection.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/npruntime/server_spawn.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/npruntime/server_initial_state.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/npruntime/server_tick.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/npruntime/client_runtime.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/npruntime/client_replica_card.cpp  # the decoded joiner debug card (ADR 0042 d5)
    ${CMAKE_CURRENT_SOURCE_DIR}/npruntime/client_replica_present_projection.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/npruntime/loadout_submit.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/npruntime/joiner_world_bridge.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/npruntime/host_session.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/npruntime/lan_discovery.cpp
    # inmatch — the one public mission lifecycle/cadence surface (session) plus
    # the SP listen bring-up/drain/frame over the mission kernel (ADR 0042 d3).
    # The np and netsim directories beneath it are wire/runtime implementation
    # modules.
    ${CMAKE_CURRENT_SOURCE_DIR}/inmatch/listen_host.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/inmatch/session.cpp
)

# engine/ is the one public include root (ADR 0040): <net/<lib>/file.h>.
target_include_directories(opennova_net
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/..
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
    ${CMAKE_CURRENT_SOURCE_DIR}/novaworld/service/lobby_session.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/novaworld/service/lobby_update.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/novaworld/service/host_repository.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/novaworld/service/unknown_tracker.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/novaworld/service/connection/registry.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/novaworld/service/connection/manager.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/novaworld/service/db/sqlite.cpp
)

target_link_libraries(opennova_novaworld_service
    PUBLIC opennova_net
    PRIVATE opennova_sqlite
)

target_compile_features(opennova_novaworld_service PUBLIC cxx_std_17)

set_target_properties(opennova_novaworld_service PROPERTIES
    POSITION_INDEPENDENT_CODE ON
)
