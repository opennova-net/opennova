# The net stack: two STATIC targets along the one seam that survives at
# target level.
#
#   opennova_net — the WIRE: novacrypto, napi, the npwire game wire codec
#     (+ the LAN discovery codec and the datagram-socket seam), and the
#     novaworld SESSION and GATE legs (client-side matchmaking). The in-match
#     session and replication moved to engine/runtime (ADR 0043 d4); this
#     target links nothing above base. The godot/src links opennova_runtime,
#     which PUBLIC-links this — never the service.
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
    # session framing, capture decode, the LAN discovery codec
    ${CMAKE_CURRENT_SOURCE_DIR}/npwire/wire/entity_class.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/npwire/wire/ingame_body_codec.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/npwire/wire/ingame_decode.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/npwire/wire/ingame_encode.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/npwire/wire/visible_players.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/npwire/wire/emote_wire.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/npwire/wire/squad_messages.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/npwire/session/session_hello.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/npwire/session/cs_config.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/npwire/session/outgoing_packets.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/npwire/session/nw_session_framing.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/npwire/session/protocol_message.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/npwire/replay/wire_capture.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/npwire/replay/serverlog_decode.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/npwire/lan_discovery.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/npwire/session_vars.cpp # the session-variable list (0x60 server info)
    # novaworld session leg — NWU session establishment above the npwire
    # framing: the client session state machine, the session-protocol
    # classifier, join identity, the lobby CU var-list codec
    ${CMAKE_CURRENT_SOURCE_DIR}/novaworld/session/session_protocol.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/novaworld/session/client_session.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/novaworld/session/join_identity.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/novaworld/session/lobby_update.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/novaworld/session/lobby_vars.cpp
    # novaworld gate leg — first-contact + account entry: novaworld_gate
    # probe/response, registration URL cipher, HTTP login flow, GSB browser data
    ${CMAKE_CURRENT_SOURCE_DIR}/novaworld/gate/gate_metrics.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/novaworld/gate/gate_probe.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/novaworld/gate/gate_response.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/novaworld/gate/registration_url.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/novaworld/gate/http_login.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/novaworld/server_browser.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/novaworld/gate/http_flow.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/novaworld/gate/gsb.cpp
)

# engine/ is the one public include root (ADR 0040): <net/<lib>/file.h>.
target_include_directories(opennova_net
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/..
)

# The chain (ADR 0043 d4): net -> base -> formats -> io (all PUBLIC). net is
# wire only; the in-match session and replication live in runtime/ above it,
# and opennova_net never links opennova_runtime (link_graph_check.py).
target_link_libraries(opennova_net PUBLIC opennova_base)

target_compile_features(opennova_net PUBLIC cxx_std_17)

set_target_properties(opennova_net PROPERTIES
    POSITION_INDEPENDENT_CODE ON
)

# service — the server-side lobby/persistence leg: lobby sessions and updates,
# host repository, connection registry/manager, unknown tracker, sqlite. The
# ONLY target that links sqlite (see the guardrail note at the top).
add_library(opennova_novaworld_service STATIC
    ${CMAKE_CURRENT_SOURCE_DIR}/novaworld/service/lobby_session.cpp
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
