cmake_minimum_required(VERSION 3.20)

project(opennova_godot LANGUAGES C CXX)

add_subdirectory(../../third_party/godot-cpp godot-cpp)
# sqlite is only needed to DEFINE the novaworld service target inside engine/;
# the extension links opennova_net (wire codec + session/gate legs + netsim +
# npruntime), NEVER opennova_novaworld_service, so no sqlite or service code
# ends up in the binary.
add_subdirectory(../../third_party/sqlite opennova_sqlite)
# Dear ImGui + the engine's ImGui pass (ADR 0039) build in every flavour (ONED's
# run surface needs them in the release-mode dev zip too). The game's dev-tool
# windows ride the GDExtension flavour: every template_debug build (the editor,
# scripts/build_godot.sh, PR CI, debug packaging) carries them; the
# template_release DLL compiles them out and the game's release export strips the
# imgui-godot addon. GODOTCPP_TARGET is defined by now (godot-cpp's option
# default or the -D on the command line).
add_subdirectory(../../third_party/imgui opennova_imgui)
option(OPENNOVA_DEVTOOLS "Build the game's in-engine dev-tool windows (Dear ImGui)" ON)
if(GODOTCPP_TARGET STREQUAL "template_release")
    set(OPENNOVA_DEVTOOLS OFF)
endif()
# The engine (Shape A group targets — see engine/CMakeLists.txt).
add_subdirectory(../../engine opennova_engine)

set(TARGET_NAME "opennova")

add_library(${TARGET_NAME} SHARED
    register_types.cpp
    network/nova_world_client.cpp
    network/nova_world_host.cpp
    network/nova_world_identity.cpp
    network/nwu_lobby_session.cpp
    network/item_replication_catalog_adapter.cpp
    network/nova_udp_pump.cpp
    network/nova_lan_session.cpp
    network/nova_net_protocol.cpp
    network/nova_net_session_policy.cpp
    terrain/nova_terrain.cpp
    terrain/nova_terrain_surface_inputs.cpp
    terrain/nova_terrain_static_shadow_rasterizer.cpp
    terrain/nova_terrain_tile_cache_device.cpp
    terrain/nova_terrain_data.cpp
    terrain/nova_terrain_foliage_def.cpp
    terrain/nova_terrain_foliage_map.cpp
    terrain/nova_foliage_dispatcher.cpp
    terrain/nova_terrain_tile_entry.cpp
    terrain/nova_terrain_tile_info.cpp
    cbin/cbin_credits_resource.cpp
    cbin/nova_credits_player.cpp
    env/nova_env_keyframe.cpp
    env/env_file.cpp
    env/nova_color_smoother.cpp
    env/nova_mission_environment.cpp
    mission/nova_mission_catalog.cpp
    mission/nova_mission_object_placer.cpp
    mission/nova_mission_object_placer_static_sources.cpp
    env/nova_celestial.cpp
    env/nova_environment_cube_capture.cpp
    env/nova_sky_dome.cpp
    env/nova_slot_shadow.cpp
    env/nova_sun_shadow.cpp
    env/nova_water.cpp
    env/nova_weather.cpp
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
    lights/nova_light_scene.cpp
    particle/nova_effect_scene.cpp
    particle/nova_particle_compositor.cpp
    particle/nova_particle_renderer.cpp
    render/nova_framefx.cpp
    object/nova_entity_index.cpp
    object/nova_object_model.cpp
    object/nova_object_model_anim.cpp
    object/nova_object_model_materials.cpp
    object/nova_object_model_scene.cpp
    object/nova_object_data.cpp
    object/nova_object_data_bind.cpp
    object/nova_object_data_state.cpp
    object/nova_object_data_materials.cpp
    object/nova_object_data_geometry.cpp
    object/nova_object_data_panm_inspection.cpp
    object/nova_object_data_runtime_eval.cpp
    object/nova_object_shader_cache.cpp
    object/nova_item_database.cpp
    object/nova_weapon_database.cpp
    object/nova_avatar_database.cpp
    object/nova_skeletal_anim.cpp
    hud/hud_overlay.cpp
    devtools/nova_frame_stats.cpp
    devtools/imgui_pass_node.cpp
    devtools/nova_dev_tools.cpp
    devtools/nova_oned_ui.cpp
    hud/nova_hud_pos.cpp
    mission/nova_mission_data.cpp
    simulation/nova_present_applier.cpp
    simulation/nova_present_applier_wire.cpp
    simulation/nova_present_stats.cpp
    simulation/nova_inmatch_session_values.cpp
    simulation/nova_wire_present_pass.cpp
    simulation/nova_simulation.cpp
    simulation/nova_simulation_assets.cpp
    simulation/nova_simulation_bind.cpp
    simulation/nova_simulation_frame.cpp
    simulation/nova_simulation_frame_stats.cpp
    simulation/nova_simulation_net.cpp
    simulation/nova_simulation_occlusion.cpp
    simulation/nova_simulation_player.cpp
    simulation/nova_simulation_player_loadout.cpp
    simulation/nova_simulation_deploy_screen.cpp
    simulation/nova_simulation_end_round.cpp
    simulation/nova_simulation_player_view.cpp
    simulation/nova_simulation_player_weapon.cpp
    simulation/nova_simulation_present.cpp
    simulation/nova_simulation_scars.cpp
    simulation/nova_simulation_hud_feeds.cpp
    world/nova_scar_presenter.cpp
    wac/nova_wac_program.cpp
    lwf/nova_lwf_data.cpp
    lwf/nova_wav_loader.cpp
    audio/nova_ambient_mixer.cpp
    audio/nova_sound_selector.cpp
    dbf/nova_dbf_data.cpp
    rtxt/rtxt_string_file.cpp
    util/engine_caches.cpp
    util/nova_data_format.cpp
    util/nova_paths.cpp
    util/nova_process.cpp
    util/pcx_texture_bridge.cpp
    util/texture_path_resolver.cpp
    resource_index/nova_resource_root.cpp
    pff/nova_pff_archive.cpp
    fnt/nova_fnt_resource.cpp
    audio/nova_sbf_bank.cpp
    audio/nova_sbf_audio_stream.cpp
    audio/nova_sbf_audio_stream_playback.cpp
    audio/nova_music_script.cpp
    audio/nova_music_director.cpp
    mnu/nova_mnu_document.cpp
    mnu/nova_mnu_document_state.cpp
    mnu/mns_stylesheet.cpp
    mnu/nova_menu_frame.cpp
    mnu/nova_menu_audio.cpp
    mnu/menu_video_underlay.cpp
    mnu/nova_controls_model.cpp
)

target_include_directories(${TARGET_NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
)

# opennova_net PUBLIC-pulls the whole sim chain (runtime -> base -> formats ->
# io); opennova_runtime is named for the direct sim/format consumption in the
# bindings. The service target is deliberately absent (see the sqlite note
# above); scripts/lint/link_graph_check.py enforces it.
target_link_libraries(${TARGET_NAME} PRIVATE
    godot-cpp
    opennova_runtime
    opennova_net
)

if(WIN32)
    target_link_libraries(${TARGET_NAME} PRIVATE iphlpapi)
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
