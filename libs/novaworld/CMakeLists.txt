# The NovaWorld matchmaking service lib: three targets along the protocol
# legs (headers stay flat under include/novaworld/; sources group by leg
# under src/). The game wire protocol lives below this in libs/npwire
# (ADR 0019): matchmaking sits ON the wire base. opennova_novaworld remains
# as an INTERFACE umbrella linking every leg plus opennova_npwire so coarse
# consumers and tests keep working; narrow consumers (the apps, the Godot
# client) link only the legs they use - notably, only the service leg links
# sqlite.

# session - NWU session establishment above the npwire framing: the client
# session state machine, the session-protocol classifier, join identity, and
# the lobby CU var-list codec. (hello/keys/framing moved to npwire.)
add_library(opennova_novaworld_session STATIC
    src/session/session_protocol.cpp
    src/session/client_session.cpp
    src/session/join_identity.cpp
    src/session/lobby_vars.cpp
)

target_include_directories(opennova_novaworld_session
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(opennova_novaworld_session
    PUBLIC opennova_npwire opennova_novacrypto opennova_napi
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
    PUBLIC opennova_io opennova_novacrypto
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
    PUBLIC opennova_npwire opennova_novacrypto opennova_napi opennova_io
    PRIVATE opennova_sqlite
)

target_compile_features(opennova_novaworld_service PUBLIC cxx_std_17)

# Umbrella: everything, for tests and coarse consumers.
add_library(opennova_novaworld INTERFACE)

target_link_libraries(opennova_novaworld INTERFACE
    opennova_npwire
    opennova_novaworld_session
    opennova_novaworld_gate
    opennova_novaworld_service
)
