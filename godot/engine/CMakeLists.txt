cmake_minimum_required(VERSION 3.20)

project(opennova_godot LANGUAGES C CXX)

add_subdirectory(../../third_party/godot-cpp godot-cpp)
add_subdirectory(../../libs/pcx opennova_pcx)
add_subdirectory(../../libs/fnt opennova_fnt)
add_subdirectory(../../libs/foliage opennova_foliage)
add_subdirectory(../../libs/til opennova_til)
add_subdirectory(../../libs/tpj opennova_tpj)
add_subdirectory(../../libs/trn opennova_trn)
add_subdirectory(../../libs/cpt opennova_cpt)
add_subdirectory(../../libs/env opennova_env)
add_subdirectory(../../libs/threedi opennova_threedi)
add_subdirectory(../../libs/pff opennova_pff)
add_subdirectory(../../libs/scr opennova_scr)
add_subdirectory(../../libs/bfc1 opennova_bfc1)
add_subdirectory(../../libs/vfs opennova_vfs)
add_subdirectory(../../libs/rtxt opennova_rtxt)
add_subdirectory(../../libs/ase opennova_ase)
add_subdirectory(../../libs/tdp opennova_tdp)
add_subdirectory(../../libs/oed opennova_oed)
add_subdirectory(../../libs/renderer opennova_renderer)
add_subdirectory(../../libs/cbin opennova_cbin)
add_subdirectory(../../libs/terrain opennova_terrain)
add_subdirectory(../../libs/resource_index opennova_resource_index)
add_subdirectory(../../libs/sbf opennova_sbf)
add_subdirectory(../../libs/scr opennova_scr)
add_subdirectory(../../libs/mus opennova_mus)
add_subdirectory(../../libs/mnu_xml opennova_mnu_xml)
add_subdirectory(../../libs/mnu opennova_mnu)
add_subdirectory(../../libs/mns opennova_mns)
add_subdirectory(../../libs/def opennova_def)
add_subdirectory(../../libs/mission opennova_mission)

set(TARGET_NAME "opennova")

add_library(${TARGET_NAME} SHARED
    register_types.cpp
    terrain/nova_terrain.cpp
    terrain/nova_terrain_build_job.cpp
    terrain/nova_terrain_data.cpp
    terrain/nova_terrain_builder.cpp
    terrain/nova_terrain_foliage_def.cpp
    terrain/nova_terrain_foliage_map.cpp
    terrain/nova_terrain_surface_map.cpp
    terrain/nova_foliage_dispatcher.cpp
    terrain/nova_terrain_tile_entry.cpp
    terrain/nova_terrain_tile_info.cpp
    terrain/nova_terrain_tile_overlay.cpp
    terrain/trn_resource_format.cpp
    terrain/cpt_resource_format.cpp
    terrain/til_resource_format.cpp
    cbin/cbin_credits_resource.cpp
    cbin/kda_resource_format.cpp
    cbin/nova_credits_player.cpp
    env/nova_env_keyframe.cpp
    env/env_file.cpp
    object/nova_object_data.cpp
    object/nova_object_shader_cache.cpp
    object/nova_item_database.cpp
    mission/nova_mission_data.cpp
    rtxt/rtxt_string_file.cpp
    rtxt/rtxt_resource_format.cpp
    util/nova_data_format.cpp
    util/nova_paths.cpp
    util/nova_texture_format.cpp
    util/pcx_texture_bridge.cpp
    util/texture_path_resolver.cpp
    resource_index/nova_resource_index.cpp
    resource_index/nova_resource_root.cpp
    editor/opennova_editor_plugin.cpp
    fnt/nova_fnt_resource.cpp
    fnt/fnt_resource_format.cpp
    fnt/fnt_import_plugin.cpp
    audio/nova_sbf_bank.cpp
    audio/nova_sbf_audio_stream.cpp
    audio/nova_sbf_audio_stream_playback.cpp
    audio/sbf_resource_format.cpp
    audio/nova_music_script.cpp
    audio/nova_music_director.cpp
    audio/mus_resource_format.cpp
    mnu/nova_mnu_document.cpp
    mnu/mns_stylesheet.cpp
    mnu/mnu_resource_format.cpp
    mnu/mns_resource_format.cpp
    mnu/nova_mnu_screen.cpp
    mnu/nova_mnu_label.cpp
    mnu/nova_mnu_button.cpp
    mnu/nova_mnu_checkbox.cpp
    mnu/nova_mnu_widget_behavior.cpp
    mnu/nova_mnu_edit.cpp
    mnu/nova_mnu_multiline_edit.cpp
    mnu/nova_mnu_goto.cpp
    mnu/nova_mnu_list.cpp
    mnu/nova_mnu_multi.cpp
    mnu/nova_mnu_spinlist.cpp
    mnu/nova_mnu_scroll.cpp
    mnu/nova_mnu_combo.cpp
    mnu/nova_mnu_table.cpp
    mnu/nova_mnu_map.cpp
    mnu/nova_mnu_globe.cpp
    mnu/nova_mnu_marquee.cpp
    mnu/nova_mnu_builder.cpp
    mnu/nova_mnu_menu.cpp
)

target_include_directories(${TARGET_NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(${TARGET_NAME} PRIVATE
    godot-cpp
    opennova_pcx
    opennova_fnt
    opennova_foliage
    opennova_til
    opennova_tpj
    opennova_trn
    opennova_cpt
    opennova_cbin
    opennova_env
    opennova_threedi
    opennova_pff
    opennova_rtxt
    opennova_ase
    opennova_tdp
    opennova_oed
    opennova_renderer
    opennova_terrain
    opennova_vfs
    opennova_resource_index
    opennova_sbf
    opennova_scr
    opennova_mus
    opennova_mnu_xml
    opennova_mnu
    opennova_mns
    opennova_def
    opennova_mission
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
