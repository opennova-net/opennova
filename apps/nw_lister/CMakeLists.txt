# nw-lister: advertises one server listing on a NovaWorld master without
# running the game. Protocol and crypto come from the net group; the app owns
# its sockets, the HTTP transport, timing and logging.
add_library(opennova_nw_lister_core STATIC
    listing.cpp
    log.cpp
    udp.cpp
    http_client.cpp
)

target_include_directories(opennova_nw_lister_core
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(opennova_nw_lister_core
    PUBLIC
        opennova_net
)

if(WIN32)
    target_link_libraries(opennova_nw_lister_core PUBLIC ws2_32)
endif()

target_compile_features(opennova_nw_lister_core PUBLIC cxx_std_17)

add_executable(opennova_nw_lister
    main.cpp
)

target_link_libraries(opennova_nw_lister
    PRIVATE
        opennova_nw_lister_core
)

target_compile_features(opennova_nw_lister PRIVATE cxx_std_17)
set_target_properties(opennova_nw_lister PROPERTIES OUTPUT_NAME "nw-lister")
