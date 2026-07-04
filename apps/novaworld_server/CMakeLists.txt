set(NOVAWORLD_SERVER_SOURCES
    main.cpp
    server_config.cpp
    gate_listener.cpp
    nw_udp_listener.cpp
)

if(BUILD_NOVAWORLD_HTTP)
    list(APPEND NOVAWORLD_SERVER_SOURCES
        http_listener.cpp
        template_engine.cpp
        session_store.cpp
        auth.cpp
        catalog_repository.cpp
        github_client.cpp
    )
endif()

add_executable(opennova_novaworld_server ${NOVAWORLD_SERVER_SOURCES})

target_link_libraries(opennova_novaworld_server
    PRIVATE
        opennova_novaworld_gate
        opennova_novaworld_service
        opennova_novaworld_session
        opennova_npwire
        opennova_npruntime
        opennova_napi
        opennova_novacrypto
        opennova_app_common
        opennova_bcrypt
)

if(BUILD_NOVAWORLD_HTTP)
    # libcurl powers the outbound GitHub tag API call in github_client.cpp.
    # Required only for the HTTP build, so lib-only / Godot builds never need it.
    find_package(CURL REQUIRED)
    target_link_libraries(opennova_novaworld_server PRIVATE Crow::Crow CURL::libcurl)
    target_compile_definitions(opennova_novaworld_server PRIVATE
        OPENNOVA_HTTP_ENABLED
    )
endif()

# Crow uses some C++17 (filesystem) and some 20-flavored bits Crow's
# CMakeLists asks for. Match Drogon's request to avoid mismatches.
target_compile_features(opennova_novaworld_server PRIVATE cxx_std_17)

set_target_properties(opennova_novaworld_server PROPERTIES
    OUTPUT_NAME "opennova-novaworld"
)

if(WIN32)
    target_link_libraries(opennova_novaworld_server PRIVATE ws2_32)
endif()
