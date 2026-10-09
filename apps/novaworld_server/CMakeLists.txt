# opennova-novaworld-server: the NovaWorld service. Every source but main.cpp
# builds as opennova_novaworld_server_core (mirroring opennova_serve_core), so
# tests drive the listeners in-process; the exe is main.cpp over it.
set(NOVAWORLD_SERVER_CORE_SOURCES
    server_config.cpp
    gate_listener.cpp
    nw_udp_listener.cpp
)

if(BUILD_NOVAWORLD_HTTP)
    list(APPEND NOVAWORLD_SERVER_CORE_SOURCES
        http_listener.cpp
        template_engine.cpp
        session_store.cpp
        auth.cpp
        catalog_repository.cpp
    )
endif()

add_library(opennova_novaworld_server_core STATIC ${NOVAWORLD_SERVER_CORE_SOURCES})

target_include_directories(opennova_novaworld_server_core
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
)

# The service target (the one sqlite consumer), the wire group (gate/session
# legs, npwire, napi, novacrypto — opennova_net), and the runtime group for
# the in-match host session the JO listener hosts peers through
# (runtime/inmatch, ADR 0043 d4).
target_link_libraries(opennova_novaworld_server_core
    PUBLIC
        opennova_novaworld_service
        opennova_runtime
        opennova_net
        opennova_app_common
        opennova_bcrypt
)

# PUBLIC so main.cpp's #ifdef OPENNOVA_HTTP_ENABLED blocks see the HTTP layer.
if(BUILD_NOVAWORLD_HTTP)
    target_link_libraries(opennova_novaworld_server_core PUBLIC Crow::Crow)
    target_compile_definitions(opennova_novaworld_server_core PUBLIC
        OPENNOVA_HTTP_ENABLED
    )
endif()

# Crow uses some C++17 (filesystem) and some 20-flavored bits Crow's
# CMakeLists asks for. Match Crow's request to avoid mismatches.
target_compile_features(opennova_novaworld_server_core PUBLIC cxx_std_17)

if(WIN32)
    target_link_libraries(opennova_novaworld_server_core PUBLIC ws2_32)
endif()

add_executable(opennova_novaworld_server main.cpp)

# The service edge is direct as well: the sqlite-containment lint
# (scripts/lint/link_graph_check.py) wants every target that reaches sqlite to
# list opennova_novaworld_service itself.
target_link_libraries(opennova_novaworld_server
    PRIVATE
        opennova_novaworld_server_core
        opennova_novaworld_service
)

target_compile_features(opennova_novaworld_server PRIVATE cxx_std_17)

set_target_properties(opennova_novaworld_server PROPERTIES
    OUTPUT_NAME "opennova-novaworld-server"
)
