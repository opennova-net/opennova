cmake_minimum_required(VERSION 3.20)

project(opennova_godot LANGUAGES CXX)

add_subdirectory(../../third_party/godot-cpp godot-cpp)
add_subdirectory(../../libs/pcx opennova_pcx)
add_subdirectory(../../libs/foliage opennova_foliage)
add_subdirectory(../../libs/til opennova_til)
add_subdirectory(../../libs/tpj opennova_tpj)
add_subdirectory(../../libs/trn opennova_trn)
add_subdirectory(../../libs/cpt opennova_cpt)
add_subdirectory(../../libs/env opennova_env)
add_subdirectory(../../libs/terrain opennova_terrain)

set(TARGET_NAME "opennova")

add_library(${TARGET_NAME} SHARED
    register_types.cpp
    terrain/nova_terrain.cpp
    terrain/nova_terrain_build_job.cpp
    terrain/nova_terrain_data.cpp
    terrain/nova_terrain_builder.cpp
    terrain/nova_terrain_foliage_def.cpp
    terrain/nova_terrain_foliage_map.cpp
    terrain/nova_foliage_dispatcher.cpp
    terrain/nova_terrain_tile_entry.cpp
    terrain/nova_terrain_tile_info.cpp
    terrain/nova_terrain_tile_overlay.cpp
    terrain/trn_resource_format.cpp
    terrain/cpt_resource_format.cpp
    terrain/til_resource_format.cpp
    env/nova_env_keyframe.cpp
    env/env_file.cpp
    util/nova_data_format.cpp
    util/nova_texture_format.cpp
    util/pcx_texture_bridge.cpp
    util/texture_path_resolver.cpp
)

target_include_directories(${TARGET_NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(${TARGET_NAME} PRIVATE
    godot-cpp
    opennova_pcx
    opennova_foliage
    opennova_til
    opennova_tpj
    opennova_trn
    opennova_cpt
    opennova_env
    opennova_terrain
)

# The bake worker thread in nova_terrain_build_job.cpp installs
# _set_se_translator so Windows SEH (access violations, div/0, etc.) surfaces
# as std::runtime_error instead of crashing with "Unknown build failure".
# That requires /EHa — only scope it to this TU so the rest of the extension
# keeps the default exception model.
if(MSVC)
    set_source_files_properties(terrain/nova_terrain_build_job.cpp
        PROPERTIES COMPILE_OPTIONS "/EHa")
endif()

get_target_property(GODOTCPP_SUFFIX godot-cpp GODOTCPP_SUFFIX)

set(OUTPUT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../bin/")

# gersemi: off
set_target_properties(
    ${TARGET_NAME}
    PROPERTIES
        CXX_STANDARD 17
        CXX_EXTENSIONS OFF
        CXX_VISIBILITY_PRESET ${GODOTCPP_SYMBOL_VISIBILITY}

        POSITION_INDEPENDENT_CODE ON

        LIBRARY_OUTPUT_DIRECTORY "$<1:${OUTPUT_DIR}>"
        RUNTIME_OUTPUT_DIRECTORY "$<1:${OUTPUT_DIR}>"
        PDB_OUTPUT_DIRECTORY     "$<1:${OUTPUT_DIR}>"

        PREFIX "lib"
        OUTPUT_NAME "${TARGET_NAME}${GODOTCPP_SUFFIX}"

        FOLDER "opennova"
)
# gersemi: on
