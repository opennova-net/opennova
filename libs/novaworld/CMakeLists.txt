# One directory, five targets along the protocol legs (headers stay flat under
# include/novaworld/; sources group by leg under src/). opennova_novaworld
# remains as an INTERFACE umbrella linking every leg so coarse consumers and
# tests keep working; narrow consumers (netsim, npruntime, the apps, the Godot
# client) link only the legs they use - notably, only the service leg links
# sqlite.

# wire - the in-game message codec: what netsim/npruntime encode/decode and
# nw_pp pretty-prints. Also home of the shared peer_addr.h POD.
add_library(opennova_novaworld_wire STATIC
    src/wire/ingame_decode.cpp
    src/wire/ingame_encode.cpp
)

target_include_directories(opennova_novaworld_wire
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_features(opennova_novaworld_wire PUBLIC cxx_std_17)

# session - NWU session establishment and framing: hello/protocol/keys, the
# NWU flag-byte message framing (protocol_message), the client session state
# machine, join identity, and the lobby CU var-list codec.
add_library(opennova_novaworld_session STATIC
    src/session/session_hello.cpp
    src/session/session_protocol.cpp
    src/session/nw_session_framing.cpp
    src/session/protocol_message.cpp
    src/session/client_session.cpp
    src/session/join_identity.cpp
    src/session/lobby_vars.cpp
)

target_include_directories(opennova_novaworld_session
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(opennova_novaworld_session
    PUBLIC opennova_novacrypto opennova_napi
)

target_compile_features(opennova_novaworld_session PUBLIC cxx_std_17)

# gate - first-contact + account entry: the novaworld_gate probe/response,
# registration URL cipher, the HTTP login flow, and the GSB server-browser data.
add_library(opennova_novaworld_gate STATIC
    src/gate/gate_probe.cpp
    src/gate/gate_response.cpp
    src/gate/registration_url.cpp
    src/gate/http_login.cpp
    src/gate/http_flow.cpp
    src/gate/gsb.cpp
)

target_include_directories(opennova_novaworld_gate
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(opennova_novaworld_gate
    PUBLIC opennova_novacrypto
)

target_compile_features(opennova_novaworld_gate PUBLIC cxx_std_17)

# service - the server-side lobby/persistence leg: lobby sessions and updates,
# host repository, connection registry/manager, unknown tracker, sqlite. The
# ONLY leg that links sqlite.
add_library(opennova_novaworld_service STATIC
    src/service/lobby_session.cpp
    src/service/lobby_update.cpp
    src/service/host_repository.cpp
    src/service/unknown_tracker.cpp
    src/service/connection/registry.cpp
    src/service/connection/manager.cpp
    src/service/db/sqlite.cpp
)

target_include_directories(opennova_novaworld_service
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(opennova_novaworld_service
    PUBLIC opennova_novaworld_wire opennova_novacrypto opennova_napi
    PRIVATE opennova_sqlite
)

target_compile_features(opennova_novaworld_service PUBLIC cxx_std_17)

# replay - capture decode + replay: the pcap/NWU decode chain (wire_capture),
# the replay timeline, and the .sph serverlog decoder.
add_library(opennova_novaworld_replay STATIC
    src/replay/wire_capture.cpp
    src/replay/replay_timeline.cpp
    src/replay/serverlog_decode.cpp
)

target_include_directories(opennova_novaworld_replay
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(opennova_novaworld_replay
    PUBLIC opennova_novaworld_wire opennova_novaworld_session
)

target_compile_features(opennova_novaworld_replay PUBLIC cxx_std_17)

# Umbrella: everything, for tests and coarse consumers.
add_library(opennova_novaworld INTERFACE)

target_link_libraries(opennova_novaworld INTERFACE
    opennova_novaworld_wire
    opennova_novaworld_session
    opennova_novaworld_gate
    opennova_novaworld_service
    opennova_novaworld_replay
)
