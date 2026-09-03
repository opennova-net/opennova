cmake_minimum_required(VERSION 3.20)

project(opennova_godot LANGUAGES C CXX)

add_subdirectory(../../third_party/godot-cpp godot-cpp)
# sqlite is only needed to DEFINE the novaworld service target inside engine/;
# the extension links opennova_runtime (which PUBLIC-links the wire-only
# opennova_net: codec + session/gate legs; the in-match session and
# replication live in runtime/, ADR 0043 d4), NEVER opennova_novaworld_service,
# so no sqlite or service code ends up in the binary.
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
    network/novaworld_client.cpp
    network/host_session_options.cpp
    network/novaworld_server_row.cpp
    network/novaworld_gate_info.cpp
    network/novaworld_server_browser.cpp
    network/ping_sweep_worker.cpp
    network/novaworld_host.cpp
    network/novaworld_identity.cpp
    network/nwu_lobby_session.cpp
    network/udp_datagram.cpp
    network/udp_pump.cpp
    network/lan_session.cpp
    network/lan_server_row.cpp
    network/net_protocol.cpp
    network/net_session_policy.cpp
    network/join_target.cpp
    terrain/terrain.cpp
    terrain/terrain_surface_inputs.cpp
    terrain/terrain_static_shadow_rasterizer.cpp
    terrain/terrain_tile_cache_device.cpp
    terrain/terrain_data.cpp
    terrain/terrain_foliage_def.cpp
    terrain/terrain_foliage_map.cpp
    terrain/foliage_dispatcher.cpp
    terrain/foliage_frame_stats.cpp
    terrain/terrain_tile_entry.cpp
    terrain/terrain_tile_info.cpp
    cbin/cbin_credits_resource.cpp
    cbin/credits_player.cpp
    env/env_keyframe.cpp
    env/env_file.cpp
    env/env_records.cpp
    env/mission_environment_overrides.cpp
    env/mission_environment.cpp
    mission/mission_catalog.cpp
    mission/mission_object_placer.cpp
    mission/mission_placement_stats.cpp
    mission/static_population_instance.cpp
    mission/static_source_records.cpp
    mission/mission_object_placer_static_sources.cpp
    env/celestial.cpp
    env/environment_cube_capture.cpp
    env/sky_dome.cpp
    env/slot_shadow.cpp
    env/sun_shadow.cpp
    env/water.cpp
    env/weather.cpp
    env/precipitation.cpp
    env/water_core.cpp
    env/star_field.cpp
    env/glare_occlusion.cpp
    particle/particle_curve_ref.cpp
    particle/particle_effect.cpp
    particle/particle_table_handles.cpp
    particle/particle_table.cpp
    particle/particle_graphic_layer.cpp
    particle/particle_def.cpp
    particle/particle_file.cpp
    lights/effect_light_director.cpp
    lights/effect_light_report.cpp
    lights/light_scene.cpp
    lights/light_spawn.cpp
    lights/light_spawn.cpp
    lights/light_spawn.cpp
    particle/effect_group_report.cpp
    particle/effect_load_report.cpp
    particle/effect_scene.cpp
    particle/effect_spawn_records.cpp
    particle/effect_world.cpp
    particle/particle_compositor.cpp
    particle/particle_renderer.cpp
    render/environment_cube_blit.cpp
    render/frame_fx.cpp
    render/object_lod_frame.cpp
    render/q3_frame_adapter.cpp
    render/q3_geometry_cache.cpp
    render/q3_source_registry.cpp
    render/slot_capture_adapter.cpp
    object/entity_index.cpp
    object/entity_ref.cpp
    object/object_model.cpp
    object/object_model_anim.cpp
    object/object_model_materials.cpp
    object/object_model_scene.cpp
    object/object_data.cpp
    object/object_data_bind.cpp
    object/object_data_state.cpp
    object/object_data_materials.cpp
    object/object_data_geometry.cpp
    object/object_data_runtime_eval.cpp
    object/object_shader_cache.cpp
    object/item_database.cpp
    object/item_records.cpp
    object/model_light.cpp
    object/model_user_point.cpp
    object/weapon_database.cpp
    object/weapon_def.cpp
    object/avatar_database.cpp
    object/avatar_records.cpp
    object/character_join_profile.cpp
    mission/player_visual_spec.cpp
    simulation/fp_viewmodel_spec.cpp
    simulation/player_aim_overlay.cpp
    simulation/player_local_view.cpp
    simulation/player_weapon_event.cpp
    simulation/destruction_events.cpp
    simulation/debug_pick_card.cpp
    simulation/hitbox_debug_report.cpp
    simulation/hud_view_records.cpp
    simulation/deploy_rows.cpp
    simulation/player_weapon_view.cpp
    simulation/player_inventory.cpp
    simulation/weapon_profile_summary.cpp
    simulation/present_event_records.cpp
    simulation/environment_snapshot.cpp
    simulation/weapon_kit_entry.cpp
    object/skeletal_anim.cpp
    hud/hud_draw_list_stats.cpp
    hud/hud_overlay.cpp
    hud/feed_row.cpp
    hud/player_hud_weapon_def.cpp
    devtools/frame_stats.cpp
    devtools/imgui_pass_node.cpp
    devtools/dev_tools.cpp
    devtools/oned_ui.cpp
    hud/hud_pos.cpp
    hud/vehicle_hud_block.cpp
    mission/mission_data.cpp
    mission/mission_records.cpp
    mission/mission_info.cpp
    mission/mission_perf_counters.cpp
    mission/mission_root.cpp
    mission/mission_setup_options.cpp
    simulation/entity_presenter.cpp
    simulation/entity_presenter_wire.cpp
    simulation/fire_presenter.cpp
    simulation/destruction_presenter.cpp
    simulation/throwable_presenter.cpp
    simulation/present_stats.cpp
    simulation/entity_card.cpp
    simulation/entity_row.cpp
    simulation/end_round_state.cpp
    simulation/inmatch_session_values.cpp
    simulation/simulation.cpp
    simulation/simulation_assets.cpp
    simulation/simulation_bind.cpp
    simulation/simulation_frame.cpp
    simulation/simulation_frame_stats.cpp
    simulation/simulation_ai_debug.cpp
    simulation/simulation_net.cpp
    simulation/simulation_occlusion.cpp
    simulation/simulation_player.cpp
    simulation/simulation_player_loadout.cpp
    simulation/simulation_deploy_screen.cpp
    simulation/simulation_end_round.cpp
    simulation/simulation_player_view.cpp
    simulation/simulation_player_weapon.cpp
    simulation/simulation_present.cpp
    simulation/simulation_scars.cpp
    simulation/simulation_hud_feeds.cpp
    world/item_effect_director.cpp
    world/occlusion_frame.cpp
    world/game_world.cpp
    world/game_world_frame.cpp
    world/game_world_load.cpp
    world/game_world_effects.cpp
    world/session_drive.cpp
    world/world_view.cpp
    world/load_timeline.cpp
    world/loading_screen_info.cpp
    world/runtime_perf_counters.cpp
    world/resource_root_resolver.cpp
    world/scar_draw_list.cpp
    world/scar_presenter.cpp
    wac/wac_program.cpp
    lwf/lwf_data.cpp
    lwf/wav_loader.cpp
    audio/ambient_mixer.cpp
    audio/sound_selector.cpp
    audio/ambient_layer.cpp
    audio/sound_bank.cpp
    audio/mission_audio_records.cpp
    audio/mission_audio.cpp
    dbf/dbf_data.cpp
    rtxt/rtxt_string_file.cpp
    util/engine_caches.cpp
    util/data_format.cpp
    util/paths.cpp
    util/process.cpp
    util/pcx_texture_bridge.cpp
    util/texture_path_resolver.cpp
    resource_index/resource_root.cpp
    resource_index/launch_flags.cpp
    pff/pff_document.cpp
    fnt/fnt_resource.cpp
    audio/sbf_bank.cpp
    audio/sbf_audio_stream.cpp
    audio/sbf_audio_stream_playback.cpp
    audio/music_script.cpp
    audio/music_director.cpp
    audio/music_pair_names.cpp
    mnu/mnu_document.cpp
    mnu/mnu_rows.cpp
    mnu/mns_stylesheet.cpp
    mnu/menu_draw_list_stats.cpp
    mnu/menu_frame.cpp
    mnu/menu_audio.cpp
    mnu/menu_video_underlay.cpp
    mnu/controls_model.cpp
    player/first_person_arms_witness.cpp
    player/gameplay_camera.cpp
    player/local_player_presenter.cpp
    player/local_player_visuals.cpp
    player/player_input_router.cpp
    player/player_move_intent.cpp
    player/player_spawn_loadout.cpp
    player/player_viewmodel_def.cpp
    player/player_viewmodel_rig.cpp
    player/player_weapon_effects.cpp
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
