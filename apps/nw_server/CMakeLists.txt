# nw-server — the headless in-match game HOST (libs/npruntime P6).
#
# godot-cpp guard (structural): godot-cpp is built by a SEPARATE SCons tree
# (third_party/godot-cpp/SConstruct) and is NEVER add_subdirectory'd into this CMake graph, so it is
# not a linkable target here. This executable links ONLY opennova_* static libs (all strictly
# Godot-agnostic, libs/CLAUDE.md) + apps/common, which makes it structurally incapable of pulling in
# godot-cpp. There is no toggle to set — keep the link list opennova_*-only and that invariant holds.
add_executable(opennova_nw_server
    main.cpp
)

target_link_libraries(opennova_nw_server
    PRIVATE
        opennova_npruntime   # the in-match runtime (Server_TickUpdate / handle_server_datagram /
                             # frame_in_match_s2c); PUBLIC-pulls netsim / novaworld / world transitively
        opennova_mission     # MissionDocument + promote_mission (load + build the authoritative World)
        opennova_app_common  # net_sockets (real UDP); PUBLIC-links ws2_32 on WIN32
)

target_compile_features(opennova_nw_server PRIVATE cxx_std_17)
set_target_properties(opennova_nw_server PROPERTIES OUTPUT_NAME "nw-server")

if(WIN32)
    # belt-and-braces: opennova_app_common already PUBLIC-links ws2_32, but main.cpp uses net_sockets
    # (+ net_datagram_socket) directly.
    target_link_libraries(opennova_nw_server PRIVATE ws2_32)
endif()
