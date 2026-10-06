# opennova-nw-lister: lists one server on a NovaWorld master without the game. The NWU session, the host
# role, the lobby HTTP machines and the remote-admin codec are the engine's (net/novaworld,
# net/admin); the app owns the sockets (apps/common), the listing sources (the file and the admin
# poll thread, or an embedder's live one: opennova-serve links this core) and the log.
find_package(Threads REQUIRED)

add_library(opennova_nw_lister_core STATIC
    admin_feed.cpp
    console_log.cpp
    file_listing_source.cpp
    lister.cpp
    listing.cpp
    policy.cpp
)

target_include_directories(opennova_nw_lister_core
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(opennova_nw_lister_core
    PUBLIC
        opennova_app_common
        opennova_net
        Threads::Threads
)

target_compile_features(opennova_nw_lister_core PUBLIC cxx_std_17)

add_executable(opennova_nw_lister
    main.cpp
)

target_link_libraries(opennova_nw_lister
    PRIVATE
        opennova_nw_lister_core
)

target_compile_features(opennova_nw_lister PRIVATE cxx_std_17)
set_target_properties(opennova_nw_lister PROPERTIES OUTPUT_NAME "opennova-nw-lister")
