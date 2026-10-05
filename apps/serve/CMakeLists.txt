# opennova-serve: the headless game server (ADR 0051) -- the game's Serve Only host with no
# Godot, configured by the retail host file. The mission kernel, the host role, the session and
# the host-file reader are the engine's (runtime/); the NovaWorld listing is opennova-nw-lister's
# Lister over a live source (ADR 0051 d6); the app owns the mount call order, the socket
# (apps/common), the wall clock and the console. godot-cpp is never on this graph: the link list
# below is opennova_* only.
add_library(opennova_serve_core STATIC
    serve_listing.cpp
    server.cpp
)

target_include_directories(opennova_serve_core
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(opennova_serve_core
    PUBLIC
        opennova_runtime     # MissionKernel, inmatch::HostRole / Session, the host file
        opennova_nw_lister_core # the NovaWorld listing (Lister, the log sink, the credentials)
        opennova_app_common  # the UDP socket (PUBLIC-links ws2_32 on Windows)
)

target_compile_features(opennova_serve_core PUBLIC cxx_std_17)

add_executable(opennova_serve
    main.cpp
)

target_link_libraries(opennova_serve
    PRIVATE
        opennova_serve_core
)

target_compile_features(opennova_serve PRIVATE cxx_std_17)
set_target_properties(opennova_serve PROPERTIES OUTPUT_NAME "opennova-serve")
