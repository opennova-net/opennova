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
    ${CMAKE_CURRENT_SOURCE_DIR}/gameprofile/src/gameprofile.c
    ${CMAKE_CURRENT_SOURCE_DIR}/gameprofile/src/required_resources.c
    # pcapio — pcap/pcapng reading (shared by capture-driven tests and tools)
    ${CMAKE_CURRENT_SOURCE_DIR}/pcapio/src/pcap_reader.cpp
    # refs — cross-resource reference extraction + the whole-root RefGraph
    ${CMAKE_CURRENT_SOURCE_DIR}/refs/src/refs.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/refs/src/refs_env.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/refs/src/refs_cbin.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/refs/src/refs_def.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/refs/src/refs_avatar.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/refs/src/refs_threedi.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/refs/src/refs_mission.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/refs/src/refs_mnu.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/refs/src/ref_graph.cpp
    # resource_index — the mounted resource directory index
    ${CMAKE_CURRENT_SOURCE_DIR}/resource_index/src/resource_index.cpp
    # vfs — the engine-faithful PFF+loose+expansion mount stack
    ${CMAKE_CURRENT_SOURCE_DIR}/vfs/src/vfs.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/vfs/src/vfs_decode.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/vfs/src/vfs_capi.cpp
)

target_include_directories(opennova_base
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/gameprofile/include
        ${CMAKE_CURRENT_SOURCE_DIR}/pcapio/include
        ${CMAKE_CURRENT_SOURCE_DIR}/refs/include
        ${CMAKE_CURRENT_SOURCE_DIR}/resource_index/include
        ${CMAKE_CURRENT_SOURCE_DIR}/vfs/include
)

target_link_libraries(opennova_base PUBLIC opennova_io opennova_formats)

target_compile_features(opennova_base PUBLIC cxx_std_17)

set_target_properties(opennova_base PROPERTIES
    POSITION_INDEPENDENT_CODE ON
)
