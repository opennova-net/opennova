# opennova_base — the engine base services (vfs, resource_index, gameprofile,
# pcapio, refs), one STATIC group target (Shape A). The header-only leaves
# (base/io, base/oned_edit) keep their own INTERFACE targets in their
# subdirectories.
#
# base sits ABOVE formats in the Shape A chain (the deliberate inversion): vfs
# consumes pff/scr/bfc1 and refs consumes env/cbin/def/avatars/threedi/
# mission_format/mnu, so this group PUBLIC-links opennova_formats.

add_library(opennova_base STATIC
    # gameprofile — game identity + required-resource tables
    ${CMAKE_CURRENT_SOURCE_DIR}/gameprofile/gameprofile.c
    ${CMAKE_CURRENT_SOURCE_DIR}/gameprofile/required_resources.c
    # pcapio — pcap/pcapng reading (shared by capture-driven tests and tools)
    ${CMAKE_CURRENT_SOURCE_DIR}/pcapio/pcap_reader.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/pcapio/pcap_writer.cpp
    # refs — cross-resource reference extraction + the whole-root RefGraph
    ${CMAKE_CURRENT_SOURCE_DIR}/refs/refs.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/refs/refs_env.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/refs/refs_cbin.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/refs/refs_def.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/refs/refs_avatar.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/refs/refs_threedi.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/refs/refs_mission.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/refs/refs_mnu.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/refs/ref_graph.cpp
    # resource_index — the mounted resource directory index
    ${CMAKE_CURRENT_SOURCE_DIR}/resource_index/resource_index.cpp
    # vfs — the engine-faithful PFF+loose+expansion mount stack
    ${CMAKE_CURRENT_SOURCE_DIR}/vfs/vfs.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/vfs/vfs_decode.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/vfs/vfs_capi.cpp
)

target_include_directories(opennova_base
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(opennova_base PUBLIC opennova_io opennova_formats)

target_compile_features(opennova_base PUBLIC cxx_std_17)

set_target_properties(opennova_base PROPERTIES
    POSITION_INDEPENDENT_CODE ON
)
