# opennova_base — the engine base services (vfs, resource_index, gameprofile,
# pcapio), one STATIC group target (Shape A). The header-only base/io leaf
# keeps its own INTERFACE target in its subdirectory.
#
# base sits ABOVE formats in the Shape A chain (the deliberate inversion): vfs
# consumes pff/scr/bfc1, so this group PUBLIC-links opennova_formats.

add_library(opennova_base STATIC
    # gameprofile — game identity + required-resource tables
    ${CMAKE_CURRENT_SOURCE_DIR}/gameprofile/gameprofile.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/gameprofile/required_resources.cpp
    # pcapio — pcap/pcapng reading (shared by capture-driven tests and tools)
    ${CMAKE_CURRENT_SOURCE_DIR}/pcapio/pcap_reader.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/pcapio/pcap_writer.cpp
    # resource_index — the mounted resource directory index
    ${CMAKE_CURRENT_SOURCE_DIR}/resource_index/resource_index.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/resource_index/boot_policy.cpp
    # vfs — the engine-faithful PFF+loose+expansion mount stack
    ${CMAKE_CURRENT_SOURCE_DIR}/vfs/vfs.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/vfs/vfs_decode.cpp
)

# engine/ is the one public include root (ADR 0040): <base/<lib>/file.h>.
target_include_directories(opennova_base
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/..
)

target_link_libraries(opennova_base PUBLIC opennova_io opennova_formats)

target_compile_features(opennova_base PUBLIC cxx_std_17)

set_target_properties(opennova_base PROPERTIES
    POSITION_INDEPENDENT_CODE ON
)
