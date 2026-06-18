add_library(opennova_novaworld STATIC
    src/registration_url.cpp
    src/gate_response.cpp
    src/gate_probe.cpp
    src/lobby_update.cpp
    src/session_hello.cpp
    src/protocol_message.cpp
    src/client_session.cpp
    src/game_session.cpp
    src/game_server_runtime.cpp
    src/gsb.cpp
    src/join_identity.cpp
    src/replication_min.cpp
    src/ingame_decode.cpp
    src/ingame_encode.cpp
    src/wire_capture.cpp
    src/replay_timeline.cpp
    src/serverlog_decode.cpp
    src/connection/registry.cpp
    src/connection/manager.cpp
    src/db/sqlite.cpp
    src/lobby_session.cpp
    src/host_repository.cpp
    src/unknown_tracker.cpp
    src/http_login.cpp
)

target_include_directories(opennova_novaworld
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(opennova_novaworld
    PUBLIC opennova_novacrypto opennova_napi
    PRIVATE opennova_sqlite
)

target_compile_features(opennova_novaworld PUBLIC cxx_std_17)
