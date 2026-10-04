add_library(opennova_app_common STATIC
    net_http.cpp
    net_sockets.cpp
)

target_include_directories(opennova_app_common
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
)

target_compile_features(opennova_app_common PUBLIC cxx_std_17)

# net_datagram_socket.h carries npwire's peer_addr.h; link the net group
# explicitly rather than free-riding on consumers' transitive include dirs.
target_link_libraries(opennova_app_common PUBLIC opennova_net)

if(WIN32)
    target_link_libraries(opennova_app_common PUBLIC ws2_32)
endif()
