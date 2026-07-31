cmake_minimum_required(VERSION 3.20)

project(opennova_godot LANGUAGES C CXX)

add_subdirectory(../../third_party/godot-cpp godot-cpp)
add_subdirectory(../../libs/io opennova_io)
add_subdirectory(../../libs/pcx opennova_pcx)
add_subdirectory(../../libs/fnt opennova_fnt)
add_subdirectory(../../libs/foliage opennova_foliage)
add_subdirectory(../../libs/til opennova_til)
add_subdirectory(../../libs/tpj opennova_tpj)
add_subdirectory(../../libs/trn opennova_trn)
add_subdirectory(../../libs/cpt opennova_cpt)
add_subdirectory(../../libs/env opennova_env)
add_subdirectory(../../libs/threedi opennova_threedi)
add_subdirectory(../../libs/bad opennova_bad)
add_subdirectory(../../libs/anim opennova_anim)
add_subdirectory(../../libs/pff opennova_pff)
add_subdirectory(../../libs/gameprofile opennova_gameprofile)
add_subdirectory(../../libs/scr opennova_scr)
add_subdirectory(../../libs/bfc1 opennova_bfc1)
add_subdirectory(../../libs/vfs opennova_vfs)
add_subdirectory(../../libs/rtxt opennova_rtxt)
add_subdirectory(../../libs/ase opennova_ase)
add_subdirectory(../../libs/tdp opennova_tdp)
add_subdirectory(../../libs/oed opennova_oed)
add_subdirectory(../../libs/renderer opennova_renderer)
add_subdirectory(../../libs/particle opennova_particle)
add_subdirectory(../../libs/cbin opennova_cbin)
add_subdirectory(../../libs/terrain_query opennova_terrain_query)
add_subdirectory(../../libs/terrain opennova_terrain)
add_subdirectory(../../libs/resource_index opennova_resource_index)
add_subdirectory(../../libs/sbf opennova_sbf)
add_subdirectory(../../libs/mus opennova_mus)
add_subdirectory(../../libs/mnu opennova_mnu)
add_subdirectory(../../libs/mns opennova_mns)
add_subdirectory(../../libs/def opennova_def)
add_subdirectory(../../libs/avatars opennova_avatars)
add_subdirectory(../../libs/refs opennova_refs)
add_subdirectory(../../libs/world opennova_world)
add_subdirectory(../../libs/wac opennova_wac)
add_subdirectory(../../libs/mission opennova_mission)
add_subdirectory(../../libs/lwf opennova_lwf)
add_subdirectory(../../libs/dbf opennova_dbf)
add_subdirectory(../../libs/audio opennova_audio)
add_subdirectory(../../libs/oned_edit opennova_oned_edit)
add_subdirectory(../../libs/controls opennova_controls)
add_subdirectory(../../libs/playersav opennova_playersav)
add_subdirectory(../../libs/novacrypto opennova_novacrypto)
add_subdirectory(../../libs/napi opennova_napi)
# sqlite is only needed to DEFINE the novaworld service-leg target; the
# extension links the game wire lib (npwire) plus the client legs
# (session/gate), so no sqlite
# or service code ends up in the binary.
add_subdirectory(../../third_party/sqlite opennova_sqlite)
add_subdirectory(../../libs/npwire opennova_npwire)
add_subdirectory(../../libs/novaworld opennova_novaworld)
# netsim: the in-match net seam (NetSystem + World<->wire bridge + loopback).
add_subdirectory(../../libs/netsim opennova_netsim)
# npruntime: the IDA-faithful in-match NAPI NP runtime (host owner loop + headless client).
# PUBLIC-links netsim/novaworld/world/mission (all already linked below). The Godot binding
# (nova_simulation) drives it as a thin adapter; protocol/crypto stay in libs (ADR 0009-0012).
add_subdirectory(../../libs/npruntime opennova_npruntime)

# Family link-interface groups (LIBS-2, ADR 0024): this root is the canonical
# whole-family consumer — the extension links the family targets below instead
# of naming every terrain/audio member.
include(${CMAKE_CURRENT_SOURCE_DIR}/../../libs/families.cmake)

set(TARGET_NAME "opennova")

add_library(${TARGET_NAME} SHARED
    register_types.cpp
    network/nova_world_client.cpp
    network/nova_world_host.cpp
    network/nwu_lobby_session.cpp
    network/nova_net_client.cpp
    network/nova_udp_pump.cpp
    network/nova_lan_session.cpp
    terrain/nova_terrain.cpp
    terrain/nova_terrain_surface_inputs.cpp
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
    env/nova_color_smoother.cpp
    env/nova_weather_core.cpp
    env/nova_water_core.cpp
    env/nova_star_field.cpp
    env/nova_glare_occlusion.cpp
    particle/nova_particle_curve_ref.cpp
    particle/nova_particle_effect.cpp
    particle/nova_particle_table_handles.cpp
    particle/nova_particle_table.cpp
    particle/nova_particle_graphic_layer.cpp
    particle/nova_particle_def.cpp
    particle/nova_particle_file.cpp
    particle/nova_effect_scene.cpp
    particle/nova_particle_emitter.cpp
    particle/nova_particle_compositor.cpp
    particle/nova_particle_renderer.cpp
    particle/ptl_resource_format.cpp
    object/nova_object_data.cpp
    object/nova_object_data_bind.cpp
    object/nova_object_data_state.cpp
    object/nova_object_data_materials.cpp
    object/nova_object_data_geometry.cpp
    object/nova_object_data_panm_edit.cpp
    object/nova_object_data_runtime_eval.cpp
    object/nova_object_shader_cache.cpp
    object/nova_item_database.cpp
    object/nova_weapon_database.cpp
    object/nova_avatar_database.cpp
    object/nova_skeletal_anim.cpp
    hud/nova_hud_pos.cpp
    mission/nova_mission_data.cpp
    simulation/infantry_root_motion.cpp
    simulation/nova_present_applier.cpp
    simulation/nova_present_applier_wire.cpp
    simulation/nova_simulation.cpp
    simulation/nova_simulation_assets.cpp
    simulation/nova_simulation_bind.cpp
    simulation/nova_simulation_net.cpp
    simulation/nova_simulation_occlusion.cpp
    simulation/nova_simulation_player.cpp
    simulation/nova_simulation_present.cpp
    wac/nova_wac_program.cpp
    lwf/nova_lwf_data.cpp
    lwf/nova_wav_loader.cpp
    audio/nova_ambient_mixer.cpp
    audio/nova_sound_selector.cpp
    dbf/nova_dbf_data.cpp
    rtxt/rtxt_string_file.cpp
    rtxt/rtxt_resource_format.cpp
    util/engine_caches.cpp
    util/nova_data_format.cpp
    util/nova_paths.cpp
    util/nova_texture_format.cpp
    util/pcx_texture_bridge.cpp
    util/texture_path_resolver.cpp
    resource_index/nova_resource_index.cpp
    refs/nova_reference_index.cpp
    resource_index/nova_resource_root.cpp
    pff/nova_pff_archive.cpp
    editor/opennova_editor_plugin.cpp
    editor/nova_edit_history.cpp
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
    mnu/nova_mnu_document_state.cpp
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
    mnu/mnu_item_cell.cpp
    mnu/nova_mnu_builder.cpp
    mnu/nova_mnu_menu.cpp
    mnu/nova_controls_model.cpp
)

target_include_directories(${TARGET_NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(${TARGET_NAME} PRIVATE
    godot-cpp
    opennova_pcx
    opennova_refs
    opennova_fnt
    opennova_terrain_family
    opennova_cbin
    opennova_env
    opennova_threedi
    opennova_bad
    opennova_anim
    opennova_pff
    opennova_gameprofile
    opennova_rtxt
    opennova_ase
    opennova_tdp
    opennova_oed
    opennova_renderer
    opennova_particle
    opennova_vfs
    opennova_resource_index
    opennova_scr
    opennova_mnu
    opennova_mns
    opennova_def
    opennova_avatars
    opennova_world
    opennova_wac
    opennova_mission
    opennova_audio_family
    opennova_oned_edit
    opennova_controls
    opennova_playersav
    opennova_novacrypto
    opennova_napi
    opennova_npwire
    opennova_novaworld_session
    opennova_novaworld_gate
    opennova_netsim
    opennova_npruntime
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
