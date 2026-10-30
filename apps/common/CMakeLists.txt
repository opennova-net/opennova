add_library(opennova_app_common STATIC
    admin_tcp_server.cpp
    command_runner.cpp   # a command-line tool from an argv array, no shell (base/proc)
    update_tcp_server.cpp # the updater's TCP listener over net/update's machine (ADR 0061)
)

target_include_directories(opennova_app_common
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
)

target_compile_features(opennova_app_common PUBLIC cxx_std_17)

# The listeners and net_datagram_socket.h ride the engine's platform sockets
# (<net/socket/socket.h>, which carries ws2_32 on WIN32) and npwire's peer_addr.h;
# link the net group explicitly rather than free-riding on consumers' transitive
# include dirs.
target_link_libraries(opennova_app_common PUBLIC opennova_net)
