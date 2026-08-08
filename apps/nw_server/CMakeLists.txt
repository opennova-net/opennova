# nw-server — the headless in-match game HOST (engine/net/npruntime P6).
#
# godot-cpp guard (structural): godot-cpp is built by a SEPARATE SCons tree
# (third_party/godot-cpp/SConstruct) and is NEVER add_subdirectory'd into this CMake graph, so it is
# not a linkable target here. This executable links ONLY opennova_* static libs (all strictly
# Godot-agnostic, engine/CLAUDE.md) + apps/common, which makes it structurally incapable of pulling in
# godot-cpp. There is no toggle to set — keep the link list opennova_*-only and that invariant holds.
add_library(opennova_nw_server_startup STATIC
    environment_startup.cpp
    wac_startup.cpp
)
target_include_directories(opennova_nw_server_startup
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(opennova_nw_server_startup
    PUBLIC opennova_runtime)
target_compile_features(opennova_nw_server_startup PUBLIC cxx_std_17)

add_executable(opennova_nw_server main.cpp)

target_link_libraries(opennova_nw_server
    PRIVATE
        opennova_net         # npruntime's in-match runtime (Server_TickUpdate / handle_server_datagram /
                             # frame_in_match_s2c); PUBLIC-pulls the runtime world transitively
        opennova_runtime     # MissionDocument + promote_mission (load + build the authoritative World)
        opennova_nw_server_startup # mission-selected ENV/BMS/WAC -> authoritative phase-2 owner
        opennova_app_common  # net_sockets (real UDP); PUBLIC-links ws2_32 on WIN32
)

target_compile_features(opennova_nw_server PRIVATE cxx_std_17)
set_target_properties(opennova_nw_server PROPERTIES OUTPUT_NAME "nw-server")

if(WIN32)
    # belt-and-braces: opennova_app_common already PUBLIC-links ws2_32, but main.cpp uses net_sockets
    # (+ net_datagram_socket) directly.
    target_link_libraries(opennova_nw_server PRIVATE ws2_32)
endif()
