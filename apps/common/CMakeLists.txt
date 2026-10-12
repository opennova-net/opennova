add_library(opennova_app_common STATIC
    admin_tcp_server.cpp
    net_http.cpp
    net_sockets.cpp
    process.cpp          # child processes from an argv array, no shell (the hosted-server supervisor)
)

target_include_directories(opennova_app_common
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
)

target_compile_features(opennova_app_common PUBLIC cxx_std_17)

# net_datagram_socket.h carries npwire's peer_addr.h; link the net group
# explicitly rather than free-riding on consumers' transitive include dirs.
# process.cpp blocks signals around its fork with pthread_sigmask.
find_package(Threads REQUIRED)
target_link_libraries(opennova_app_common PUBLIC opennova_net Threads::Threads)

if(WIN32)
    target_link_libraries(opennova_app_common PUBLIC ws2_32)
endif()
