# opennova-serve: the headless game server (ADR 0051) -- the game's Serve Only host with no
# Godot, configured by the retail host file. The mission kernel, the host role, the session and
# the host-file reader are the engine's (runtime/); the NWU session, the host role and the lobby
# HTTP machines its NovaWorld listing drives are the engine's too (net/novaworld). The app owns
# the listing's driver over the live match (the Lister and ServeListing, ADR 0051 d6), the mount
# call order, the sockets (apps/common), the wall clock and the console. godot-cpp is never on
# this graph: the link list below is opennova_* only.
find_package(Threads REQUIRED)

add_library(opennova_serve_core STATIC
    console_log.cpp      # the log sink: stderr, the secrets masked
    credentials.cpp      # the --credentials file
    destination_policy.cpp
    lister.cpp           # the NovaWorld listing's driver: the gate, the NWU session, the login and HOSTKEY
    serve_listing.cpp
    server.cpp
    server_logs.cpp      # the working-directory file seam and the log switches (ADR 0051 PR7)
    status_console.cpp   # the status page as text on stdout, stdin as its chat input
)

target_include_directories(opennova_serve_core
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(opennova_serve_core
    PUBLIC
        opennova_runtime     # MissionKernel, inmatch::HostRole / Session, the host file
        opennova_net         # the NWU session, the host role, the lobby HTTP machines
        opennova_app_common  # the UDP socket and the HTTP exchange (PUBLIC-links ws2_32 on Windows)
        Threads::Threads     # the lister's HTTP exchange runs on a worker thread
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

# The install, host-file, loose-root and credentials paths arrive as arguments: without a UTF-8
# manifest Windows hands them over in the ANSI code page, and the engine opens files from UTF-8
# strings (io/os_path.h), so a non-ASCII path fails to open. The same manifest as opennova-3di
# (MSVC merges a .manifest source into the one it embeds).
if(MSVC)
    target_sources(opennova_serve PRIVATE ${CMAKE_SOURCE_DIR}/apps/3di/utf8.manifest)
endif()
